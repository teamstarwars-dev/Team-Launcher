#include "social.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line (traces SDK, TL_SOCIAL_DEBUG)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h> // GetCurrentProcessId (SetGameWindowPid)
#endif

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef TL_HAS_DISCORD_SOCIAL
// DISCORDPP_IMPLEMENTATION doit etre defini dans UNE seule unite de
// compilation : l'en-tete embarque le corps des fonctions de liaison.
#define DISCORDPP_IMPLEMENTATION
#include "discordpp.h"

#include <memory>
#endif

namespace tl::social {

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

#ifdef TL_HAS_DISCORD_SOCIAL

struct Ctx {
    std::shared_ptr<discordpp::Client> client;
    discordpp::Client::Status status = discordpp::Client::Status::Disconnected;
    bool started = false;
    bool loggingIn = false;
    std::string error;
    // Le SDK marshalle ses callbacks via discordpp::PostTask, qui exige un
    // « contexte de synchronisation ». Sans lui, PostTask declenche un
    // assert(s_synchronizationContext) — un point d'arret, donc un
    // plantage immediat du launcher (constate au premier clic sur « Lier
    // mon compte », code d'exception 0x80000003).
    //
    // On empile ici les taches postees depuis n'importe quel thread du
    // SDK, et pump() les execute sur le thread de la frame. Les callbacks
    // tournent donc toujours au meme endroit que l'interface.
    std::vector<std::function<void()>> tasks;

    // Messagerie : historique par correspondant (ordre croissant), non-lus,
    // conversations dont l'historique a deja ete demande, derniere erreur
    // d'envoi asynchrone, compteur d'identifiants locaux (« local-N » pour
    // les messages optimistes en attente de confirmation).
    std::map<std::string, std::vector<Message>> threads;
    std::map<std::string, int> unread;
    std::set<std::string> historyAsked;
    std::string sendError;
    std::uint64_t pendingSeq = 0;

    // Appel vocal prive : un seul a la fois.
    enum class CallDir { None, Out, In };
    CallDir callDir = CallDir::None;
    bool callActive = false;      // voix etablie (sinon : sonnerie)
    std::uint64_t callLobby = 0;  // lobby a deux, 0 si aucun
    std::string callPeer;         // userId texte du correspondant
    std::string callPeerName;
    std::string callSecret;       // secret du lobby, echange en DM
    bool callMuted = false;
    bool callDeaf = false;
    long long callSince = 0;      // voix etablie a cette heure unix
    long long callDeadline = 0;   // timeout sortant (unix), 0 si aucun
    std::string callNotice;       // dernier evenement, consomme par l'UI
    std::uint64_t pendingLeave = 0; // lobby a quitter (balaye par pump)
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

std::uint64_t app_id() {
    // Meme application que la Rich Presence : un seul ID, donc un seul
    // asset de logo et une seule identite cote Discord.
    const std::string s = DataStore::settings.discordAppId;
    if (s.empty()) return 0;
    try {
        return std::stoull(s);
    } catch (const std::exception&) {
        return 0;
    }
}

long long unix_now() {
    return static_cast<long long>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

// Identifiant texte -> Discord : les Friend.id sont des userId numeriques
// en texte. Zero = invalide.
std::uint64_t parse_user_id(const std::string& s) {
    if (s.empty()) return 0;
    try {
        return std::stoull(s);
    } catch (const std::exception&) {
        return 0;
    }
}

// Identifiant du compte courant (0 si inconnu). GetCurrentUser est
// deprecie au profit de GetCurrentUserV2 (optionnel).
std::uint64_t self_id(const std::shared_ptr<discordpp::Client>& client) {
    if (!client) return 0;
    const auto me = client->GetCurrentUserV2();
    if (!me) return 0;
    return me->Id();
}

// Secret de lobby : 16 hexas aleatoires, prefixe lisible pour le
// diagnostic. Jamais derivable des userId.
std::string make_secret() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s = "tlc-";
    for (int i = 0; i < 16; i++) s += hex[rng() & 0xF];
    return s;
}

// Remise a zero de l'etat d'appel. Verrou tenu. La notice est conservee
// (l'appelant la pose avant ou apres) et pendingLeave est pose a part.
void reset_call_locked(Ctx& c) {
    c.callDir = Ctx::CallDir::None;
    c.callActive = false;
    c.callLobby = 0;
    c.callPeer.clear();
    c.callPeerName.clear();
    c.callSecret.clear();
    c.callMuted = false;
    c.callDeaf = false;
    c.callSince = 0;
    c.callDeadline = 0;
}

// Quitte le vocal puis detruit le lobby, sans bloquer. Sans verrou tenu.
void leave_lobby_async(const std::shared_ptr<discordpp::Client>& client,
                       std::uint64_t lobby) {
    if (!client || lobby == 0) return;
    client->EndCall(lobby, []() {});
    client->LeaveLobby(lobby, [](discordpp::ClientResult) {});
}

// Signalisation d'appel en DM (contenu lisible dans Discord, metadonnees
// pour le launcher). Sans verrou tenu.
void send_signal(const std::shared_ptr<discordpp::Client>& client,
                 std::uint64_t peer, const std::string& kind,
                 const std::string& secret,
                 discordpp::Client::SendUserMessageCallback done = nullptr) {
    if (!client || peer == 0) return;
    const char* body = "";
    if (kind == "invite")
        body = "Appel vocal entrant (TeamLauncher) — décroche dans le "
               "launcher.";
    else if (kind == "accept")
        body = "Appel accepté.";
    else if (kind == "decline")
        body = "Appel refusé.";
    else if (kind == "hangup")
        body = "Appel terminé.";
    const std::unordered_map<std::string, std::string> meta = {
        {"tlcall", kind}, {"tlsecret", secret}};
    if (done)
        client->SendUserMessageWithMetadata(peer, body, meta, done);
    else
        client->SendUserMessageWithMetadata(
            peer, body, meta,
            [](discordpp::ClientResult, std::uint64_t) {});
}

// Traite un DM de signalisation. Verrou tenu, jamais d'appel SDK ici :
// les departs de lobby passent par pendingLeave (balaye par pump).
void handle_signal_locked(const std::shared_ptr<discordpp::Client>& client,
                          std::uint64_t sender, const std::string& kind,
                          const std::string& secret) {
    auto& c = ctx();
    const std::string from = std::to_string(sender);
    if (kind == "invite") {
        // Deja en appel : on ignore (un seul a la fois). Le delai de
        // sonnerie cote appelant expirera.
        if (c.callDir != Ctx::CallDir::None) return;
        std::string name = "Un ami";
        for (const auto& rel : client->GetRelationships()) {
            const auto user = rel.User();
            if (user && std::to_string(rel.Id()) == from) {
                name = user->Username();
                break;
            }
        }
        c.callDir = Ctx::CallDir::In;
        c.callActive = false;
        c.callLobby = 0;
        c.callPeer = from;
        c.callPeerName = name;
        c.callSecret = secret;
        return;
    }
    // Les autres signaux n'ont de sens que dans l'appel courant.
    if (c.callDir == Ctx::CallDir::None || c.callPeer != from ||
        c.callSecret != secret || secret.empty())
        return;
    if (kind == "decline") {
        if (c.callDir == Ctx::CallDir::Out) {
            c.pendingLeave = c.callLobby;
            reset_call_locked(c);
            c.callNotice = "Appel refusé.";
        }
    } else if (kind == "hangup") {
        c.pendingLeave = c.callLobby;
        reset_call_locked(c);
        c.callNotice = "Appel terminé.";
    }
    // « accept » : le correspondant arrive, l'etat actif suit l'entree
    // au lobby (LobbyMemberAdded). Rien a faire ici.
}

Presence map_presence(discordpp::StatusType st) {
    switch (st) {
        case discordpp::StatusType::Online:
        case discordpp::StatusType::Idle:
        case discordpp::StatusType::Dnd:
        case discordpp::StatusType::Streaming:
            return Presence::Online;
        default:
            break;
    }
    return Presence::Offline;
}

// Integre un message du SDK dans le fil du correspondant. Verrou tenu.
// L'expediteur est soi quand AuthorId == compte courant ; le correspondant
// est alors le destinataire, sinon l'auteur. Les doublons (historique puis
// evenement live, ou echo de nos propres envois) sont ignores ; un envoi
// optimiste en attente est confirme plutot que duplique.
void ingest_locked(const std::shared_ptr<discordpp::Client>& client,
                   discordpp::MessageHandle h) {
    auto& c = ctx();
    // Signalisation d'appel : jamais affichee, jamais comptee en non-lus.
    const auto meta = h.Metadata();
    const auto kit = meta.find("tlcall");
    if (kit != meta.end()) {
        std::string secret;
        const auto sit = meta.find("tlsecret");
        if (sit != meta.end()) secret = sit->second;
        handle_signal_locked(client, h.AuthorId(), kit->second, secret);
        return;
    }
    const std::uint64_t me = self_id(client);
    const std::uint64_t author = h.AuthorId();
    const bool mine = (author == me);
    const std::string partner =
        std::to_string(mine ? h.RecipientId() : author);
    auto& thread = c.threads[partner];
    const std::string id = std::to_string(h.Id());
    for (const auto& m : thread)
        if (m.id == id) return; // deja connu
    if (mine) {
        // Echo de notre envoi : confirmer l'optimiste de meme texte.
        const std::string text = h.Content();
        for (auto& m : thread) {
            if (m.pending && m.mine && m.text == text) {
                m.pending = false;
                m.id = id;
                m.unixTime = static_cast<long long>(h.SentTimestamp());
                return;
            }
        }
    }
    Message m;
    m.id = id;
    m.authorId = std::to_string(author);
    m.text = h.Content();
    m.unixTime = static_cast<long long>(h.SentTimestamp());
    m.mine = mine;
    m.pending = false;
    thread.push_back(std::move(m));
    // Borne memoire : 200 derniers par fil (comme le plafond du SDK).
    while (thread.size() > 200) thread.erase(thread.begin());
    if (!mine) c.unread[partner]++;
}

#endif // TL_HAS_DISCORD_SOCIAL

const char* not_ready_reason() {
#ifndef TL_HAS_DISCORD_SOCIAL
    return "Le SDK social de Discord n'est pas intégré à cette version. Il se "
           "récupère sur le portail développeur Discord, puis se lie au "
           "launcher : les amis et les messages privés ne passent pas par le "
           "canal local utilisé pour la Rich Presence, qui ne sait que "
           "déclarer une activité.";
#else
    if (!DataStore::settings.discordEnabled)
        return "L'intégration Discord est désactivée dans les paramètres.";
    if (app_id() == 0)
        return "Aucun identifiant d'application Discord configuré.";
    if (!ctx().started)
        return "Liaison Discord non démarrée.";
    if (!ctx().error.empty()) return ctx().error.c_str();
    switch (ctx().status) {
        case discordpp::Client::Status::Disconnected:
            return "Pas encore connecté à Discord. Lie ton compte pour voir "
                   "tes amis.";
        case discordpp::Client::Status::Connecting:
        case discordpp::Client::Status::HttpWait:
            return "Connexion à Discord en cours…";
        case discordpp::Client::Status::Reconnecting:
            return "Reconnexion à Discord…";
        case discordpp::Client::Status::Disconnecting:
            return "Déconnexion en cours…";
        default: break;
    }
    return "Discord n'est pas joignable. Vérifie qu'il est lancé et connecté.";
#endif
}

bool refuse(std::string* errOut) {
    if (errOut) *errOut = not_ready_reason();
    return false;
}

} // namespace

Status status() {
    std::lock_guard<std::mutex> lk(lock());
    Status s;
    s.providerName = "Discord";
#ifdef TL_HAS_DISCORD_SOCIAL
    s.ready = ctx().started && DataStore::settings.discordEnabled &&
              ctx().status == discordpp::Client::Status::Ready;
#else
    s.ready = false;
#endif
    // Vocal prive 1:1 : disponible des que le client est pret (lobby a
    // deux + StartCall, audio par defaut gere par le SDK).
    s.voiceAvailable = s.ready;
#ifdef TL_HAS_DISCORD_SOCIAL
    // Proposer la liaison n'a de sens que si le SDK est la, l'integration
    // activee et une application configuree. Sinon le bouton ne pourrait
    // rien faire.
    s.canLink = !s.ready && ctx().started && DataStore::settings.discordEnabled &&
                app_id() != 0;
#endif
    if (!s.ready) s.detail = not_ready_reason();
    return s;
}

std::vector<Friend> friends() {
    if (!status().ready) return {};
#ifdef TL_HAS_DISCORD_SOCIAL
    std::lock_guard<std::mutex> lk(lock());
    auto& c = ctx();
    if (!c.client) return {};

    std::vector<Friend> out;
    for (const auto& rel : c.client->GetRelationships()) {
        const auto user = rel.User();
        if (!user) continue; // relation sans utilisateur resolu : on l'ignore
        Friend f;
        f.id = std::to_string(rel.Id());
        f.name = user->Username();
        // Presence reelle du SDK (en ligne / absent / ne pas deranger /
        // streaming) ; l'activite de jeu bascule en « en jeu ».
        f.presence = map_presence(user->Status());
        if (const auto act = user->GameActivity()) {
            f.presence = Presence::Playing;
            f.activity = "Joue à " + act->Name();
        }
        const auto uit = c.unread.find(f.id);
        f.unread = (uit != c.unread.end()) ? uit->second : 0;
        out.push_back(std::move(f));
    }
    std::sort(out.begin(), out.end(), [](const Friend& a, const Friend& b) {
        return a.name < b.name;
    });
    return out;
#else
    return {};
#endif
}

std::vector<Message> conversation(const std::string& friendId) {
    if (!status().ready || friendId.empty()) return {};
#ifdef TL_HAS_DISCORD_SOCIAL
    // Plafond documente par Discord pour les fonctionnalites de
    // communication : traffic limite (envois plafonnes par application),
    // acces soumis a approbation sur la page « Acces aux communications »
    // du portail. La messagerie du launcher reste donc un usage modere.
    std::shared_ptr<discordpp::Client> client;
    bool askHistory = false;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.client) return {};
        client = c.client;
        c.unread[friendId] = 0; // conversation affichee = lue
        if (c.historyAsked.insert(friendId).second) askHistory = true;
    }
    if (askHistory && parse_user_id(friendId) != 0) {
        const std::uint64_t rid = parse_user_id(friendId);
        // Historique (50 derniers) : remplace le fil sauf les optimistes
        // locaux encore en attente, tries du plus ancien au plus recent.
        client->GetUserMessagesWithLimit(
            rid, 50,
            [client, friendId](discordpp::ClientResult r,
                               std::vector<discordpp::MessageHandle> msgs) {
                if (!r.Successful()) return;
                const std::uint64_t me = self_id(client);
                std::vector<Message> fetched;
                fetched.reserve(msgs.size());
                for (auto& h : msgs) {
                    const std::uint64_t author = h.AuthorId();
                    Message m;
                    m.id = std::to_string(h.Id());
                    m.authorId = std::to_string(author);
                    m.text = h.Content();
                    m.unixTime = static_cast<long long>(h.SentTimestamp());
                    m.mine = (author == me);
                    m.pending = false;
                    fetched.push_back(std::move(m));
                }
                std::sort(fetched.begin(), fetched.end(),
                          [](const Message& a, const Message& b) {
                              return a.unixTime < b.unixTime;
                          });
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                auto& thread = c.threads[friendId];
                std::vector<Message> merged;
                merged.reserve(fetched.size() + thread.size());
                for (const auto& m : fetched) {
                    bool dup = false;
                    for (const auto& t : thread)
                        if (!t.pending && t.id == m.id) {
                            dup = true;
                            break;
                        }
                    if (!dup) merged.push_back(m);
                }
                for (const auto& t : thread)
                    if (t.pending) merged.push_back(t);
                thread = std::move(merged);
                while (thread.size() > 200) thread.erase(thread.begin());
            });
    }
    std::lock_guard<std::mutex> lk(lock());
    const auto it = ctx().threads.find(friendId);
    if (it == ctx().threads.end()) return {};
    return it->second;
#else
    return {};
#endif
}

bool add_friend(const std::string& pseudo, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (pseudo.empty()) {
        if (errOut) *errOut = "Indique un pseudo.";
        return false;
    }
    return refuse(errOut);
}

bool send(const std::string& friendId, const std::string& text,
          std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (friendId.empty() || text.empty()) {
        if (errOut) *errOut = "Message vide.";
        return false;
    }
#ifdef TL_HAS_DISCORD_SOCIAL
    const std::uint64_t rid = parse_user_id(friendId);
    if (rid == 0) {
        if (errOut) *errOut = "Destinataire invalide.";
        return false;
    }
    std::shared_ptr<discordpp::Client> client;
    std::string localId;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.client) return refuse(errOut);
        client = c.client;
        // Envoi optimiste : le message apparait aussitot comme « envoi… »,
        // confirme (ou retire en cas d'echec) a la reponse du SDK.
        Message m;
        m.id = "local-" + std::to_string(++c.pendingSeq);
        m.authorId = std::to_string(self_id(client));
        m.text = text;
        m.unixTime = unix_now();
        m.mine = true;
        m.pending = true;
        localId = m.id;
        c.threads[friendId].push_back(std::move(m));
    }
    client->SendUserMessage(
        rid, text,
        [friendId, localId](discordpp::ClientResult r, std::uint64_t messageId) {
            std::lock_guard<std::mutex> lk(lock());
            auto& c = ctx();
            const auto it = c.threads.find(friendId);
            if (it == c.threads.end()) return;
            auto& thread = it->second;
            for (auto m = thread.begin(); m != thread.end(); ++m) {
                if (m->id != localId) continue;
                if (r.Successful()) {
                    m->pending = false;
                    m->id = std::to_string(messageId);
                } else {
                    thread.erase(m);
                    c.sendError = "Échec d'envoi. Réessaie.";
                }
                break;
            }
        });
    return true;
#else
    return refuse(errOut);
#endif
}

std::string take_send_error() {
    std::lock_guard<std::mutex> lk(lock());
#ifdef TL_HAS_DISCORD_SOCIAL
    std::string e = ctx().sendError;
    ctx().sendError.clear();
    return e;
#else
    return "";
#endif
}

CallInfo call_info() {
    CallInfo i;
    std::lock_guard<std::mutex> lk(lock());
#ifdef TL_HAS_DISCORD_SOCIAL
    const auto& c = ctx();
    i.idle = (c.callDir == Ctx::CallDir::None);
    i.outgoing = (c.callDir == Ctx::CallDir::Out && !c.callActive);
    i.incoming = (c.callDir == Ctx::CallDir::In && !c.callActive);
    i.active = c.callActive;
    i.peerId = c.callPeer;
    i.peerName = c.callPeerName;
    i.muted = c.callMuted;
    i.deaf = c.callDeaf;
    i.startedUnix = c.callSince;
#endif
    return i;
}

std::string take_call_notice() {
    std::lock_guard<std::mutex> lk(lock());
#ifdef TL_HAS_DISCORD_SOCIAL
    std::string n = ctx().callNotice;
    ctx().callNotice.clear();
    return n;
#else
    return "";
#endif
}

bool call_start(const std::string& friendId, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
#ifdef TL_HAS_DISCORD_SOCIAL
    const std::uint64_t rid = parse_user_id(friendId);
    if (rid == 0) {
        if (errOut) *errOut = "Destinataire invalide.";
        return false;
    }
    std::shared_ptr<discordpp::Client> client;
    std::string peerName;
    std::string secret;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.client) return refuse(errOut);
        if (c.callDir != Ctx::CallDir::None) {
            if (errOut) *errOut = "Appel déjà en cours.";
            return false;
        }
        client = c.client;
        for (const auto& rel : client->GetRelationships()) {
            const auto user = rel.User();
            if (user && std::to_string(rel.Id()) == friendId) {
                peerName = user->Username();
                break;
            }
        }
        if (peerName.empty()) peerName = "Un ami";
        secret = make_secret();
        c.callDir = Ctx::CallDir::Out;
        c.callPeer = friendId;
        c.callPeerName = peerName;
        c.callSecret = secret;
    }
    // Lobby a deux puis demarrage du vocal : le correspondant nous
    // rejoint via l'invitation (secret). L'audio passe par les
    // peripheriques par defaut, geres par le SDK.
    client->CreateOrJoinLobby(
        secret, [client, friendId, peerName, secret](discordpp::ClientResult r,
                                                     std::uint64_t lobbyId) {
            if (!r.Successful()) {
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir == Ctx::CallDir::Out && c.callSecret == secret) {
                    reset_call_locked(c);
                    c.callNotice = "Création de l'appel impossible.";
                }
                return;
            }
            {
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir != Ctx::CallDir::Out || c.callSecret != secret) {
                    // Annule entre-temps : quitter ce lobby au prochain
                    // balayage au lieu de le laisser orphelin.
                    c.pendingLeave = lobbyId;
                    return;
                }
                c.callLobby = lobbyId;
                c.callDeadline = unix_now() + 90;
            }
            const discordpp::Call call = client->StartCall(lobbyId);
            if (!call) {
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir == Ctx::CallDir::Out && c.callSecret == secret) {
                    c.pendingLeave = c.callLobby;
                    reset_call_locked(c);
                    c.callNotice = "Vocal indisponible.";
                }
                return;
            }
            // Invitation : si elle ne part pas, tout nettoyer.
            send_signal(client, parse_user_id(friendId), "invite", secret,
                        [client, secret](discordpp::ClientResult sr,
                                         std::uint64_t) {
                            if (sr.Successful()) return;
                            std::lock_guard<std::mutex> lk(lock());
                            auto& c = ctx();
                            if (c.callDir == Ctx::CallDir::Out &&
                                c.callSecret == secret) {
                                c.pendingLeave = c.callLobby;
                                reset_call_locked(c);
                                c.callNotice = "Invitation non envoyée.";
                            }
                        });
        });
    return true;
#else
    return refuse(errOut);
#endif
}

bool call_accept(std::string* errOut) {
    if (!status().ready) return refuse(errOut);
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::string secret, peer;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.client) return refuse(errOut);
        if (c.callDir != Ctx::CallDir::In || c.callSecret.empty()) {
            if (errOut) *errOut = "Aucun appel entrant.";
            return false;
        }
        client = c.client;
        secret = c.callSecret;
        peer = c.callPeer;
    }
    client->CreateOrJoinLobby(
        secret, [client, secret, peer](discordpp::ClientResult r,
                                       std::uint64_t lobbyId) {
            if (!r.Successful()) {
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir == Ctx::CallDir::In && c.callSecret == secret) {
                    reset_call_locked(c);
                    c.callNotice = "Connexion au vocal impossible.";
                }
                return;
            }
            const discordpp::Call call = client->StartCall(lobbyId);
            {
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir != Ctx::CallDir::In || c.callSecret != secret) {
                    // Refuse entre-temps : quitter ce lobby.
                    c.pendingLeave = lobbyId;
                    return;
                }
                if (!call) {
                    reset_call_locked(c);
                    c.callNotice = "Vocal indisponible.";
                    return;
                }
                c.callLobby = lobbyId;
                c.callActive = true;
                c.callSince = unix_now();
            }
            // Prevenir l'appelant qu'on arrive (l'etat actif suit aussi
            // l'entree au lobby de son cote).
            send_signal(client, parse_user_id(peer), "accept", secret);
        });
    return true;
#else
    return refuse(errOut);
#endif
}

bool call_decline(std::string* errOut) {
    if (!status().ready) return refuse(errOut);
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::string peer, secret;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.callDir != Ctx::CallDir::In) {
            if (errOut) *errOut = "Aucun appel entrant.";
            return false;
        }
        client = c.client;
        peer = c.callPeer;
        secret = c.callSecret;
        reset_call_locked(c);
    }
    // On n'a jamais rejoint de lobby : juste prevenir, rien a quitter.
    send_signal(client, parse_user_id(peer), "decline", secret);
    return true;
#else
    return refuse(errOut);
#endif
}

bool call_hangup(std::string* errOut) {
    if (!status().ready) return refuse(errOut);
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::string peer, secret;
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.callDir == Ctx::CallDir::None) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
        client = c.client;
        peer = c.callPeer;
        secret = c.callSecret;
        lobby = c.callLobby;
        reset_call_locked(c);
        c.pendingLeave = lobby;
    }
    send_signal(client, parse_user_id(peer), "hangup", secret);
    return true;
#else
    return refuse(errOut);
#endif
}

bool call_set_muted(bool muted, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.client || c.callLobby == 0) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
        client = c.client;
        lobby = c.callLobby;
    }
    discordpp::Call call = client->GetCall(lobby);
    if (!call) {
        if (errOut) *errOut = "Vocal non actif.";
        return false;
    }
    call.SetSelfMute(muted);
    std::lock_guard<std::mutex> lk(lock());
    ctx().callMuted = muted;
    return true;
#else
    return refuse(errOut);
#endif
}

bool call_set_deaf(bool deaf, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.client || c.callLobby == 0) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
        client = c.client;
        lobby = c.callLobby;
    }
    discordpp::Call call = client->GetCall(lobby);
    if (!call) {
        if (errOut) *errOut = "Vocal non actif.";
        return false;
    }
    call.SetSelfDeaf(deaf);
    std::lock_guard<std::mutex> lk(lock());
    ctx().callDeaf = deaf;
    return true;
#else
    return refuse(errOut);
#endif
}

bool set_blocked(const std::string& friendId, bool, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (friendId.empty()) return refuse(errOut);
    return refuse(errOut);
}

bool report(const std::string& friendId, const std::string& reason,
            std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (friendId.empty() || reason.empty()) {
        if (errOut) *errOut = "Indique un motif.";
        return false;
    }
    return refuse(errOut);
}

void start() {
#ifdef TL_HAS_DISCORD_SOCIAL
    std::lock_guard<std::mutex> lk(lock());
    auto& c = ctx();
    if (c.started) return;
    const std::uint64_t id = app_id();
    if (id == 0) return; // rien a faire sans application configuree

    // A POSER AVANT toute autre utilisation du SDK : la creation meme du
    // client peut poster une tache.
    discordpp::SetSynchronizationContext([](std::function<void()> task) {
        std::lock_guard<std::mutex> lk2(lock());
        ctx().tasks.push_back(std::move(task));
    });

    c.client = std::make_shared<discordpp::Client>();
    c.client->SetApplicationId(id);
#ifdef _WIN32
    // Requis par le flux device : sans PID, l'ouverture de l'ecran
    // d'autorisation (overlay) fait planer un assert dans le SDK.
    c.client->SetGameWindowPid(static_cast<std::int32_t>(::GetCurrentProcessId()));
#endif
    // Traces du SDK vers launcher.log (diagnostic : TL_SOCIAL_DEBUG=1).
    // Indispensable pour voir ce que fait le flux device (ouverture de
    // l'ecran d'autorisation, repli navigateur, erreurs).
    if (std::getenv("TL_SOCIAL_DEBUG")) {
        c.client->AddLogCallback(
            [](std::string msg, discordpp::LoggingSeverity sev) {
                log_line("[SocialSDK] " + std::string(discordpp::EnumToString(sev)) +
                         " : " + msg);
            },
            discordpp::LoggingSeverity::Verbose);
        // Fichier du SDK lui-meme : survit a un plantage (contrairement aux
        // callbacks, ecrits depuis ses threads).
        c.client->SetLogDir(DataStore::dir().string(),
                            discordpp::LoggingSeverity::Verbose);
    }
    // Le callback peut arriver depuis le pompage des callbacks : il doit
    // donc prendre le verrou comme tout le monde. On garde une reference
    // faible pour ne pas retenir le client si l'on s'arrete entre-temps.
    c.client->SetStatusChangedCallback(
        [](discordpp::Client::Status st, discordpp::Client::Error err,
           std::int32_t code) {
            std::lock_guard<std::mutex> lk2(lock());
            auto& c = ctx();
            c.status = st;
            if (std::getenv("TL_SOCIAL_DEBUG"))
                log_line(std::string("[SocialSDK] statut : ") +
                         std::to_string(static_cast<int>(st)) + " err=" +
                         std::to_string(static_cast<int>(err)) + " code=" +
                         std::to_string(code));
            // Echec pendant le flux de liaison (pas encore Ready) : ne pas
            // laisser le bouton « en cours » pour toujours. Hors liaison,
            // on ne touche a rien (coupure transitoire).
            if ((st == discordpp::Client::Status::Disconnected) &&
                c.loggingIn) {
                c.loggingIn = false;
                c.error = "Discord a refusé la connexion (code " +
                          std::to_string(code) +
                          "). Vérifie la configuration de l'application Discord.";
            }
        });
    c.client->SetMessageCreatedCallback([](std::uint64_t messageId) {
        // Nouveau MP (recu ou echo du notre) : GetMessageHandle puis
        // integration au fil. Appel SDK hors verrou, mutation sous verrou.
        std::shared_ptr<discordpp::Client> client;
        {
            std::lock_guard<std::mutex> lk(lock());
            if (!ctx().client) return;
            client = ctx().client;
        }
        const auto h = client->GetMessageHandle(messageId);
        if (!h) return;
        std::lock_guard<std::mutex> lk(lock());
        ingest_locked(client, std::move(*h));
    });
    // Appels vocaux : entree/sortie du correspondant au lobby (voix
    // etablie = les deux dans le lobby), et participants voix.
    c.client->SetLobbyMemberAddedCallback(
        [](std::uint64_t lobbyId, std::uint64_t memberId) {
            std::lock_guard<std::mutex> lk(lock());
            auto& c = ctx();
            if (!c.client || c.callLobby != lobbyId || lobbyId == 0) return;
            if (memberId == self_id(c.client)) return;
            if (c.callDir == Ctx::CallDir::Out && !c.callActive) {
                c.callActive = true;
                c.callSince = unix_now();
                c.callDeadline = 0;
            }
        });
    c.client->SetLobbyMemberRemovedCallback(
        [](std::uint64_t lobbyId, std::uint64_t memberId) {
            std::lock_guard<std::mutex> lk(lock());
            auto& c = ctx();
            if (!c.client || c.callLobby != lobbyId || lobbyId == 0) return;
            if (memberId == self_id(c.client)) return;
            // Le correspondant quitte : fin d'appel.
            if (c.callActive || c.callDir == Ctx::CallDir::Out) {
                c.pendingLeave = c.callLobby;
                reset_call_locked(c);
                c.callNotice = "Correspondant parti.";
            }
        });
    c.client->SetVoiceParticipantChangedCallback(
        [](std::uint64_t lobbyId, std::uint64_t memberId, bool added) {
            if (!added) return;
            std::lock_guard<std::mutex> lk(lock());
            auto& c = ctx();
            if (!c.client || c.callLobby != lobbyId || lobbyId == 0) return;
            if (memberId == self_id(c.client)) return;
            if (c.callDir == Ctx::CallDir::Out && !c.callActive) {
                c.callActive = true;
                c.callSince = unix_now();
                c.callDeadline = 0;
            }
        });
    c.started = true;
    c.error.clear();
#endif
}

bool login_in_progress() {
#ifdef TL_HAS_DISCORD_SOCIAL
    std::lock_guard<std::mutex> lk(lock());
    return ctx().loggingIn;
#else
    return false;
#endif
}

void begin_login() {
#ifdef TL_HAS_DISCORD_SOCIAL
    if (std::getenv("TL_SOCIAL_DEBUG"))
        log_line("[SocialSDK] begin_login : entree (flux Authorize).");
    std::shared_ptr<discordpp::Client> client;
    std::uint64_t id = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (!c.started || !c.client || c.loggingIn) return;
        c.loggingIn = true;
        c.error.clear();
        client = c.client;
        id = app_id();
    }
    if (!client || id == 0) {
        std::lock_guard<std::mutex> lk(lock());
        ctx().loggingIn = false;
        ctx().error = "Application Discord non configurée.";
        return;
    }
    // Flux OAuth2 desktop (recommande par Discord) : le SDK ouvre lui-meme
    // l'autorisation — overlay Discord si disponible, sinon navigateur — et
    // fait tourner son propre mini-serveur local pour le retour vers
    // http://127.0.0.1/callback (declare dans l'onglet OAuth2 du portail).
    // PKCE via le helper du SDK : challenge ici, verifier a l'echange.
    auto codeVerifier = client->CreateAuthorizationCodeVerifier();
    const std::string verifier = codeVerifier.Verifier();
    discordpp::AuthorizationArgs args;
    args.SetClientId(id);
    // Portee « communication » : amis, messages, vocal. Chaque portee
    // supplementaire est une autorisation de plus a accorder.
    args.SetScopes(discordpp::Client::GetDefaultCommunicationScopes());
    args.SetCodeChallenge(codeVerifier.Challenge());
    if (std::getenv("TL_SOCIAL_DEBUG"))
        log_line("[SocialSDK] Authorize : appel.");
    client->Authorize(
        args,
        [client, id, verifier](discordpp::ClientResult result, std::string code,
                               std::string redirectUri) {
            if (!result.Successful()) {
                if (std::getenv("TL_SOCIAL_DEBUG"))
                    log_line("[SocialSDK] Authorize : echec ou annulation.");
                std::lock_guard<std::mutex> lk(lock());
                ctx().loggingIn = false;
                ctx().error = "Autorisation Discord refusée ou annulée.";
                return;
            }
            if (std::getenv("TL_SOCIAL_DEBUG"))
                log_line("[SocialSDK] Authorize : code recu, echange...");
            // Client public : l'echange code -> jeton se fait sans serveur.
            client->GetToken(
                id, code, verifier, redirectUri,
                [client](discordpp::ClientResult r, std::string accessToken,
                         std::string /*refreshToken*/,
                         discordpp::AuthorizationTokenType tokenType,
                         std::int32_t /*expiresIn*/, std::string /*scopes*/) {
                    if (!r.Successful()) {
                        std::lock_guard<std::mutex> lk(lock());
                        ctx().loggingIn = false;
                        ctx().error = "Jeton Discord refusé.";
                        return;
                    }
                    // Le jeton est remis au SDK, qui le conserve. Le
                    // launcher ne le stocke pas lui-meme.
                    client->UpdateToken(
                        tokenType, accessToken,
                        [client](discordpp::ClientResult u) {
                            {
                                std::lock_guard<std::mutex> lk(lock());
                                ctx().loggingIn = false;
                                if (!u.Successful()) {
                                    ctx().error = "Jeton Discord refusé.";
                                    return;
                                }
                                ctx().error.clear();
                            }
                            client->Connect();
                        });
                });
        });
#endif
}

// Flux device retire : le desktop utilise Authorize (voir begin_login).

void logout() {
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        c.loggingIn = false;
        c.error.clear();
        if (c.client) {
            client = c.client;
            lobby = c.callLobby;
            reset_call_locked(c);
            // Recreer le client est le moyen sur d'oublier le jeton : il
            // n'y a pas d'API « effacer le jeton », et laisser l'ancien en
            // place reconnecterait le compte precedent au prochain
            // demarrage.
            c.client->Disconnect();
            c.client.reset();
            c.status = discordpp::Client::Status::Disconnected;
            c.started = false;
            // Session oubliee aussi cote fils et compteurs.
            c.threads.clear();
            c.unread.clear();
            c.historyAsked.clear();
            c.sendError.clear();
            c.callNotice.clear();
            c.pendingLeave = 0;
        }
    }
    leave_lobby_async(client, lobby);
#endif
}

void stop() {
#ifdef TL_HAS_DISCORD_SOCIAL
    std::shared_ptr<discordpp::Client> client;
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        client = c.client;
        lobby = c.callLobby;
        reset_call_locked(c);
        c.started = false;
        c.status = discordpp::Client::Status::Disconnected;
        c.tasks.clear(); // plus personne pour les executer
        c.threads.clear();
        c.unread.clear();
        c.historyAsked.clear();
        c.sendError.clear();
        c.callNotice.clear();
        c.pendingLeave = 0;
        // Le client est detruit ici : ses callbacks ne doivent plus courir
        // apres la fermeture.
        c.client.reset();
    }
    leave_lobby_async(client, lobby);
#else
    std::lock_guard<std::mutex> lk(lock());
#endif
}

void pump() {
#ifdef TL_HAS_DISCORD_SOCIAL
    // Le SDK exige d'etre pompe regulierement, une fois par frame selon sa
    // documentation. Hors verrou : RunCallbacks declenche nos callbacks,
    // qui prennent eux-memes le verrou.
    std::vector<std::function<void()>> todo;
    {
        std::lock_guard<std::mutex> lk(lock());
        if (!ctx().started) return;
        todo.swap(ctx().tasks);
    }
    // Les taches s'executent HORS verrou : elles rappellent nos propres
    // callbacks, qui le prennent a leur tour.
    if (!todo.empty() && std::getenv("TL_SOCIAL_DEBUG"))
        log_line("[SocialSDK] pump : " + std::to_string(todo.size()) +
                 " tache(s) postee(s).");
    for (auto& t : todo)
        if (t) t();
    discordpp::RunCallbacks();
    // Menage vocal : quitter un lobby apres reset, et timeout sortant.
    std::shared_ptr<discordpp::Client> callClient;
    std::uint64_t lobbyToLeave = 0;
    std::uint64_t signalPeer = 0;
    std::string signalSecret;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.client) {
            if (c.pendingLeave != 0) {
                callClient = c.client;
                lobbyToLeave = c.pendingLeave;
                c.pendingLeave = 0;
            }
            if (c.callDir == Ctx::CallDir::Out && !c.callActive &&
                c.callDeadline != 0 && unix_now() > c.callDeadline) {
                // Sans reponse : prevenir, puis quitter via pendingLeave.
                callClient = c.client;
                signalPeer = parse_user_id(c.callPeer);
                signalSecret = c.callSecret;
                lobbyToLeave = c.callLobby;
                reset_call_locked(c);
                c.callNotice = "Sans réponse.";
                c.pendingLeave = 0; // quitte ci-dessous, pas au prochain tour
            }
        }
    }
    if (callClient && signalPeer != 0)
        send_signal(callClient, signalPeer, "hangup", signalSecret);
    if (callClient) leave_lobby_async(callClient, lobbyToLeave);
#endif
}

} // namespace tl::social

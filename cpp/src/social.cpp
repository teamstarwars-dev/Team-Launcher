#include "social.hpp"

#include "datastore.hpp"

#include <algorithm>
#include <mutex>

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
    // Le vocal viendra dans une phase separee, une fois le texte valide en
    // usage reel. Le bouton existe deja cote interface, desactive.
    s.voiceAvailable = false;
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
        // La presence fine viendra avec la gestion des evenements ; a ce
        // stade on ne pretend pas savoir qui est en ligne plutot que de
        // l'inventer.
        f.presence = Presence::Offline;
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

std::vector<Message> conversation(const std::string&) {
    if (!status().ready) return {};
    // Messagerie : a brancher apres la liste d'amis. Rappel du plafond de
    // developpement impose par Discord — 100 envois par tranche de 2 heures
    // et PAR APPLICATION — qui impose de candidater avant toute mise en
    // service reelle.
    return {};
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
    return refuse(errOut);
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

    c.client = std::make_shared<discordpp::Client>();
    c.client->SetApplicationId(id);
    // Le callback peut arriver depuis le pompage des callbacks : il doit
    // donc prendre le verrou comme tout le monde. On garde une reference
    // faible pour ne pas retenir le client si l'on s'arrete entre-temps.
    c.client->SetStatusChangedCallback(
        [](discordpp::Client::Status st, discordpp::Client::Error,
           std::int32_t) {
            std::lock_guard<std::mutex> lk2(lock());
            ctx().status = st;
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

    discordpp::DeviceAuthorizationArgs args;
    args.SetClientId(id);
    // Portee « communication » : elle couvre les amis, les messages et le
    // vocal. On ne demande pas plus que necessaire — chaque portee
    // supplementaire est une autorisation de plus a accorder.
    args.SetScopes(discordpp::Client::GetDefaultCommunicationScopes());

    // Le SDK ouvre lui-meme l'ecran d'autorisation dans l'application
    // Discord : rien a afficher de notre cote, contrairement au flux par
    // code d'appareil de Microsoft ou le launcher montre le code.
    client->GetTokenFromDevice(
        args, [client](discordpp::ClientResult result, std::string accessToken,
                       std::string /*refreshToken*/,
                       discordpp::AuthorizationTokenType tokenType,
                       std::int32_t /*expiresIn*/, std::string /*scopes*/) {
            if (!result.Successful()) {
                std::lock_guard<std::mutex> lk(lock());
                ctx().loggingIn = false;
                ctx().error = "Liaison Discord refusée ou annulée.";
                return;
            }
            // Le jeton est remis au SDK, qui le conserve et l'utilise pour
            // se connecter. Le launcher ne le stocke pas lui-meme : moins
            // il touche a un secret, mieux c'est.
            client->UpdateToken(
                tokenType, accessToken, [client](discordpp::ClientResult r) {
                    {
                        std::lock_guard<std::mutex> lk(lock());
                        ctx().loggingIn = false;
                        if (!r.Successful()) {
                            ctx().error = "Jeton Discord refusé.";
                            return;
                        }
                        ctx().error.clear();
                    }
                    client->Connect();
                });
        });
#endif
}

void logout() {
#ifdef TL_HAS_DISCORD_SOCIAL
    std::lock_guard<std::mutex> lk(lock());
    auto& c = ctx();
    c.loggingIn = false;
    c.error.clear();
    if (c.client) {
        // Recreer le client est le moyen sur d'oublier le jeton : il n'y a
        // pas d'API « effacer le jeton », et laisser l'ancien en place
        // reconnecterait le compte precedent au prochain demarrage.
        c.client->Disconnect();
        c.client.reset();
        c.status = discordpp::Client::Status::Disconnected;
        c.started = false;
    }
#endif
}

void stop() {
    std::lock_guard<std::mutex> lk(lock());
#ifdef TL_HAS_DISCORD_SOCIAL
    auto& c = ctx();
    c.started = false;
    c.status = discordpp::Client::Status::Disconnected;
    // Le client est detruit ici : ses callbacks ne doivent plus courir
    // apres la fermeture.
    c.client.reset();
#endif
}

void pump() {
#ifdef TL_HAS_DISCORD_SOCIAL
    // Le SDK exige d'etre pompe regulierement, une fois par frame selon sa
    // documentation. Hors verrou : RunCallbacks declenche nos callbacks,
    // qui prennent eux-memes le verrou.
    {
        std::lock_guard<std::mutex> lk(lock());
        if (!ctx().started) return;
    }
    discordpp::RunCallbacks();
#endif
}

} // namespace tl::social

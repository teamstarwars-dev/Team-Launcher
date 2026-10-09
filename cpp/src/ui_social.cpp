#include "ui_internal.hpp"

#include "account.hpp"
#include "icons.hpp"
#include "social.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Page Social (16) : amis, conversation texte, blocage et signalement.
//
// L'interface est complete et independante du fournisseur. Tant que le SDK
// social de Discord n'est pas lie, la page ne montre AUCUNE donnee fictive :
// elle explique ce qui manque. Un faux fil de discussion laisserait croire
// que les messages partent.
//
// Le bouton d'appel vocal est present mais desactive, avec la mention
// « a venir » : le vocal est une phase separee, a ouvrir une fois le texte
// valide en usage reel.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct SocialState {
    std::string selected;      // ami courant (id Discord, ou "tl:"+pseudo)
    int src = 0;               // 0 = Discord, 1 = compte Team Launcher
    char draft[512] = "";      // message en cours de saisie
    char addBuf[64] = "";      // pseudo a ajouter
    char searchBuf[64] = "";   // filtre de recherche local
    std::string lastError;
    std::string threadId;      // fil suivi pour l'autoscroll
    size_t threadCount = 0;     // messages vus dans ce fil
};
SocialState S;

const char* presence_label(social::Presence p) {
    switch (p) {
        case social::Presence::Online: return "en ligne";
        case social::Presence::Playing: return "en jeu";
        case social::Presence::Offline: break;
    }
    return "hors ligne";
}

ImVec4 presence_color(social::Presence p) {
    switch (p) {
        case social::Presence::Playing: return ImVec4(0.31f, 0.78f, 0.31f, 1.0f);
        case social::Presence::Online: return kAccent;
        case social::Presence::Offline: break;
    }
    return kDim;
}

// Tri : en jeu, en ligne, hors ligne, bloques tout en bas, puis pseudo.
int presence_rank(const social::Friend& f) {
    if (f.blocked) return 3;
    switch (f.presence) {
        case social::Presence::Playing: return 0;
        case social::Presence::Online: return 1;
        case social::Presence::Offline: break;
    }
    return 2;
}

// Les ids du compte maison sont prefixes ("tl:"+pseudo) pour ne jamais
// entrer en collision avec les ids numeriques Discord. Strip a l'usage.
std::string tl_strip(const std::string& id) {
    if (id.rfind("tl:", 0) == 0) return id.substr(3);
    return id;
}

bool tl_tab() { return S.src == 1; }

// Minuscules byte a byte (suffit au filtre : on compare la frappe telle
// quelle, sans plier les accents).
std::string to_lower(std::string s) {
    for (auto& ch : s)
        ch = static_cast<char>(
            std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

// Tronque avec « … » sans jamais couper un point de code UTF-8 en deux
// (les pseudos Discord acceptent accents et emojis).
std::string fit_ellipsis(const std::string& s, float maxW) {
    if (s.empty() || maxW <= 0.0f) return s;
    if (ImGui::CalcTextSize(s.c_str()).x <= maxW) return s;
    std::string out = s;
    while (!out.empty()) {
        do {
            out.pop_back();
        } while (!out.empty() &&
                 (static_cast<unsigned char>(out.back()) & 0xC0) == 0x80);
        const std::string cand = out + "…";
        if (ImGui::CalcTextSize(cand.c_str()).x <= maxW) return cand;
    }
    return "…";
}

void draw_unavailable(const social::Status& st) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cx = ImGui::GetCursorScreenPos().x + avail.x * 0.5f;
    const float cy = ImGui::GetCursorScreenPos().y + avail.y * 0.22f;
    icons::draw(ImGui::GetWindowDrawList(), icons::Id::Account,
                ImVec2(cx - 24, cy), 48.0f,
                ImGui::ColorConvertFloat4ToU32(kDim), 2.5f);

    const char* title = tr("Messagerie indisponible", "Messaging unavailable");
    ImGui::SetCursorScreenPos(
        ImVec2(cx - ImGui::CalcTextSize(title).x * 0.5f, cy + 64));
    ImGui::TextUnformatted(title);

    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, cy + 92));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail.x - 40.0f);
    ImGui::TextUnformatted(st.detail.c_str());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    // Bouton de liaison : affiche seulement quand le SDK est la et que
    // c'est bien l'absence de compte lie qui bloque. Inutile de proposer
    // « Lier mon compte » si la bibliotheque manque.
    if (!st.canLink) return;
    ImGui::Spacing();
    ImGui::Spacing();
    const bool busy = social::login_in_progress();
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - 280.0f) * 0.5f);
    ImGui::BeginDisabled(busy);
    if (accent_button(busy ? tr("Autorisation en cours…", "Authorizing…")
                           : tr("Lier mon compte Discord",
                                "Link my Discord account"),
                      ImVec2(280, 36)))
        social::begin_login();
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    const char* hint =
        tr("Discord ouvrira lui-même l'écran d'autorisation. Il doit être "
           "lancé et connecté sur ce PC.",
           "Discord will open the authorization screen itself. It must be "
           "running and signed in on this PC.");
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() -
                          (std::min)(ImGui::CalcTextSize(hint).x, avail.x - 40.0f)) *
                         0.5f);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + avail.x - 40.0f);
    ImGui::TextUnformatted(hint);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

void draw_friend_list(const std::vector<social::Friend>& list) {
    // Recherche : filtre local instantane sur le pseudo, sans appel reseau.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(-32.0f);
    ImGui::InputTextWithHint("##searchfriend",
                             tr("Rechercher un ami", "Search friends"),
                             S.searchBuf, sizeof(S.searchBuf));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(S.searchBuf[0] == '\0');
    if (ImGui::Button("x", ImVec2(24, 0))) S.searchBuf[0] = '\0';
    ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(-90.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##addfriend", tr("Ajouter par pseudo", "Add by username"), S.addBuf,
        sizeof(S.addBuf), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const bool go = ImGui::Button(tr("Ajouter", "Add"), ImVec2(80, 0));
    if ((enter || go) && S.addBuf[0] != '\0') {
        std::string err;
        const bool ok = tl_tab() ? account::friend_request(S.addBuf, &err)
                                 : social::add_friend(S.addBuf, &err);
        if (ok)
            S.addBuf[0] = '\0';
        else
            S.lastError = err;
    }

    // Demandes recues (compte maison) : accepter ou refuser ici, sinon
    // elles s'entassent invisibles.
    if (tl_tab()) {
        for (const std::string& name : account::pending_in()) {
            ImGui::PushID(("tlpin:" + name).c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(name.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Accepter", "Accept"))) {
                std::string err;
                if (!account::friend_accept(name, &err)) S.lastError = err;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Refuser", "Decline"))) {
                std::string err;
                if (!account::friend_decline(name, &err)) S.lastError = err;
            }
            ImGui::PopID();
        }
        // Demandes envoyees : visibles aussi, sinon on croit qu'elles
        // sont parties dans le vide.
        for (const std::string& name : account::pending_out()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::Text("%s %s", tr("Demande envoyée à"), name.c_str());
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();
    if (list.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucun ami pour l'instant.",
                                    "No friends yet."));
        ImGui::PopStyleColor();
        return;
    }
    // Copie triee : statut d'abord, pseudo ensuite. La liste du SDK arrive
    // dans un ordre quelconque ; sans tri, les hors-ligne noient les
    // connectes. La recherche filtre sur le pseudo (insensible a la casse).
    const std::string query = to_lower(S.searchBuf);
    std::vector<const social::Friend*> order;
    order.reserve(list.size());
    for (const auto& f : list) {
        if (!query.empty() && to_lower(f.name).find(query) == std::string::npos)
            continue;
        order.push_back(&f);
    }
    if (order.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", query.empty()
                      ? tr("Aucun ami pour l'instant.", "No friends yet.")
                      : tr("Aucun ami trouvé.", "No friends found."));
        ImGui::PopStyleColor();
        return;
    }
    std::sort(order.begin(), order.end(), [](const social::Friend* a,
                                             const social::Friend* b) {
        const int ra = presence_rank(*a), rb = presence_rank(*b);
        if (ra != rb) return ra < rb;
        return a->name < b->name;
    });

    int lastRank = -1;
    for (const social::Friend* fp : order) {
        const auto& f = *fp;
        const int rank = presence_rank(f);
        // En-tete de section avec compteur, une seule fois par groupe.
        if (rank != lastRank) {
            lastRank = rank;
            if (rank > 0) ImGui::Spacing();
            const size_t n = std::count_if(
                order.begin(), order.end(),
                [rank](const social::Friend* o) {
                    return presence_rank(*o) == rank;
                });
            const char* label = rank == 0   ? tr("En jeu", "In game")
                                : rank == 1 ? tr("En ligne", "Online")
                                : rank == 2 ? tr("Hors ligne", "Offline")
                                            : tr("Bloqués", "Blocked");
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::Text("%s (%zu)", label, n);
            ImGui::PopStyleColor();
        }
        ImGui::PushID(f.id.c_str());
        const bool sel = f.id == S.selected;
        if (ImGui::Selectable("##f", sel, 0, ImVec2(0, 42.0f)))
            S.selected = f.id;
        const ImVec2 p0 = ImGui::GetItemRectMin();
        const ImVec2 p1 = ImGui::GetItemRectMax();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        // Tout le contenu de la ligne passe par la draw-list (aucun
        // curseur deplace) : le rendu ne depend ni du scroll ni de
        // l'etat laisse par la ligne precedente. Plus de decalage.
        const ImU32 dotCol = ImGui::ColorConvertFloat4ToU32(
            f.blocked ? kDim : presence_color(f.presence));
        dl->AddCircleFilled(ImVec2(p0.x + 11.0f, p0.y + 13.0f), 4.0f, dotCol);
        // Pastille « non lus » calee a droite : sa largeur est reservee
        // AVANT de tronquer le pseudo, pour ne plus jamais se chevaucher.
        char badge[16] = "";
        float badgeW = 0.0f;
        if (f.unread > 0) {
            std::snprintf(badge, sizeof(badge), "(%d)", f.unread);
            badgeW = ImGui::CalcTextSize(badge).x + 8.0f;
            dl->AddText(ImVec2(p1.x - badgeW, p0.y + 4.0f),
                        ImGui::ColorConvertFloat4ToU32(kAccent), badge);
        }
        const std::string name = fit_ellipsis(
            f.name, (p1.x - 8.0f - badgeW) - (p0.x + 20.0f));
        dl->AddText(ImVec2(p0.x + 20.0f, p0.y + 4.0f),
                    ImGui::ColorConvertFloat4ToU32(f.blocked ? kDim : kText),
                    name.c_str(), name.c_str() + name.size());
        // Deuxieme ligne : activite reelle (« Joue a ... ») si dispo,
        // sinon le statut. Tronquee a la largeur utile, jamais debordante.
        std::string sub;
        if (f.blocked)
            sub = tr("bloqué", "blocked");
        else if (f.presence == social::Presence::Playing && !f.activity.empty())
            sub = f.activity;
        else
            sub = tr(presence_label(f.presence));
        const std::string subFit =
            fit_ellipsis(sub, (p1.x - 8.0f) - (p0.x + 20.0f));
        dl->AddText(ImVec2(p0.x + 20.0f, p0.y + 22.0f),
                    ImGui::ColorConvertFloat4ToU32(
                        f.blocked ? kDim : presence_color(f.presence)),
                    subFit.c_str(), subFit.c_str() + subFit.size());
        ImGui::PopID();
    }
}

void draw_conversation(const std::vector<social::Friend>& list) {
    const bool tl = tl_tab();
    // Banniere d'appel entrant, quel que soit le fil affiche et l'onglet :
    // un appel manqué parce qu'on regardait l'autre source, non merci.
    {
        const auto sci = social::call_info();
        const auto aci = account::call_info();
        const bool useTl = !sci.incoming && aci.incoming;
        if (sci.incoming || aci.incoming) {
            const std::string& peerName = useTl ? aci.peer : sci.peerName;
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(tr("Appel entrant de :", "Incoming call from:"));
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::TextUnformatted(peerName.c_str());
            ImGui::SameLine();
            if (accent_button(tr("Décrocher", "Answer"), ImVec2(120, 0))) {
                std::string err;
                const bool ok = useTl ? account::call_accept(&err)
                                      : social::call_accept(&err);
                if (!ok) S.lastError = err;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Refuser", "Decline"))) {
                std::string err;
                const bool ok = useTl ? account::call_decline(&err)
                                      : social::call_decline(&err);
                if (!ok) S.lastError = err;
            }
            ImGui::Separator();
        }
    }

    const social::Friend* cur = nullptr;
    for (const auto& f : list)
        if (f.id == S.selected) cur = &f;

    if (!cur) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Choisis une conversation à gauche.",
                                    "Pick a conversation on the left."));
        ImGui::PopStyleColor();
        return;
    }
    const std::string peer = tl ? tl_strip(cur->id) : cur->id;

    // --- en-tete : nom, vocal, blocage, signalement ---
    // Etats d'appel unifies (les deux backends ont les memes champs).
    bool cIdle = true, cOut = false, cAct = false;
    std::string cPeerId, cPeerName;
    bool cMut = false, cDeaf = false, cDen = false;
    long long cSince = 0;
    bool voiceOk = false;
    if (tl) {
        const auto ci = account::call_info();
        cIdle = ci.idle;
        cOut = ci.outgoing;
        cAct = ci.active;
        cPeerId = ci.peer.empty() ? "" : ("tl:" + ci.peer);
        cPeerName = ci.peer;
        cMut = ci.muted;
        cDeaf = ci.deaf;
        cDen = ci.denoise;
        cSince = ci.startedUnix;
        voiceOk = account::status().loggedIn;
    } else {
        const auto ci = social::call_info();
        cIdle = ci.idle;
        cOut = ci.outgoing;
        cAct = ci.active;
        cPeerId = ci.peerId;
        cPeerName = ci.peerName;
        cMut = ci.muted;
        cDeaf = ci.deaf;
        cDen = ci.denoise;
        cSince = ci.startedUnix;
        voiceOk = social::status().voiceAvailable;
    }
    ImGui::TextUnformatted(cur->name.c_str());
    ImGui::SameLine();
    if (cIdle) {
        // Aucun appel : proposer d'appeler ce correspondant.
        // Cote maison, la voix passe par Discord : sans Discord lie, le
        // bouton seul mene a une erreur — on l'explique sur place.
        const bool needDiscord = tl && !social::status().ready;
        ImGui::BeginDisabled(!voiceOk || cur->blocked);
        if (ImGui::SmallButton(tr("Appel vocal", "Voice call"))) {
            std::string err;
            const bool ok = tl ? account::call_start(peer, &err)
                               : social::call_start(cur->id, &err);
            if (!ok) S.lastError = err;
        }
        ImGui::EndDisabled();
        if (needDiscord) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("(voix : lie Discord aussi)",
                                      "(voice: link Discord too)"));
            ImGui::PopStyleColor();
        }
    } else if (cPeerId == cur->id && cOut) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Appel en cours…", "Calling…"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Raccrocher", "Hang up"))) {
            std::string err;
            const bool ok =
                tl ? account::call_hangup(&err) : social::call_hangup(&err);
            if (!ok) S.lastError = err;
        }
    } else if (cPeerId == cur->id && cAct) {
        if (ImGui::SmallButton(cMut ? tr("Micro coupé", "Muted")
                                    : tr("Micro", "Mute"))) {
            std::string err;
            const bool ok = tl ? account::call_set_muted(!cMut, &err)
                               : social::call_set_muted(!cMut, &err);
            if (!ok) S.lastError = err;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(cDeaf ? tr("Son coupé", "Deafened")
                                     : tr("Son", "Deafen"))) {
            std::string err;
            const bool ok = tl ? account::call_set_deaf(!cDeaf, &err)
                               : social::call_set_deaf(!cDeaf, &err);
            if (!ok) S.lastError = err;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(cDen ? tr("Bruit réduit", "Denoised")
                                    : tr("Bruit", "Noise"))) {
            std::string err;
            const bool ok = tl ? account::call_set_denoise(!cDen, &err)
                               : social::call_set_denoise(!cDen, &err);
            if (!ok) S.lastError = err;
        }
        ImGui::SameLine();
        const long long dur =
            cSince > 0 ? std::time(nullptr) - cSince : 0;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld",
                      dur / 60 < 0 ? 0 : dur / 60, dur % 60 < 0 ? 0 : dur % 60);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Raccrocher", "Hang up"))) {
            std::string err;
            const bool ok =
                tl ? account::call_hangup(&err) : social::call_hangup(&err);
            if (!ok) S.lastError = err;
        }
    } else {
        // Appel avec un autre correspondant : le signaler sans proposer.
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("En appel avec :", "In a call with:"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(cPeerName.c_str());
    }
    ImGui::SameLine();
    if (tl) {
        if (cur->blocked) {
            if (ImGui::SmallButton(tr("Débloquer", "Unblock"))) {
                std::string err;
                if (!account::friend_unblock(peer, &err)) S.lastError = err;
            }
        } else {
            if (ImGui::SmallButton(tr("Retirer", "Remove"))) {
                std::string err;
                if (!account::friend_remove(peer, &err)) S.lastError = err;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Bloquer", "Block"))) {
                std::string err;
                if (!account::friend_block(peer, &err)) S.lastError = err;
            }
        }
    } else {
        if (ImGui::SmallButton(cur->blocked ? tr("Débloquer", "Unblock")
                                            : tr("Bloquer", "Block"))) {
            std::string err;
            if (!social::set_blocked(cur->id, !cur->blocked, &err))
                S.lastError = err;
        }
    }
    ImGui::Separator();

    // --- fil ---
    const float composerH = 78.0f;
    ImGui::BeginChild("##thread",
                      ImVec2(0, ImGui::GetContentRegionAvail().y - composerH));
    // Les messages du compte maison sont convertis au format d'affichage
    // commun (memes champs) : le rendu ci-dessous reste unique.
    const std::vector<social::Message> dmsgs =
        tl ? std::vector<social::Message>() : social::conversation(cur->id);
    std::vector<social::Message> tmsgs;
    if (tl) {
        for (const auto& m : account::conversation(peer)) {
            social::Message c;
            c.id = std::to_string(m.id);
            c.authorId = m.sender;
            c.text = m.text;
            c.unixTime = m.unixTime;
            c.mine = m.mine;
            c.pending = m.pending;
            tmsgs.push_back(std::move(c));
        }
    }
    const auto& msgs = tl ? tmsgs : dmsgs;
    if (msgs.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucun message.", "No messages."));
        ImGui::PopStyleColor();
    }
    for (const auto& m : msgs) {
        ImGui::PushStyleColor(ImGuiCol_Text, m.mine ? kAccent : kDim);
        ImGui::TextUnformatted(m.mine ? tr("Moi", "Me") : cur->name.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::TextUnformatted(m.text.c_str());
        ImGui::PopTextWrapPos();
        if (m.pending) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("envoi…", "sending…"));
            ImGui::PopStyleColor();
        }
    }
    // Autoscroll : changement de fil ou nouveau message -> bas du fil.
    if (cur->id != S.threadId || msgs.size() != S.threadCount) {
        S.threadId = cur->id;
        S.threadCount = msgs.size();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();

    // --- saisie ---
    ImGui::BeginDisabled(cur->blocked);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(-110.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##draft",
        cur->blocked ? tr("Conversation bloquée", "Conversation blocked")
                     : tr("Écrire un message…", "Write a message…"),
        S.draft, sizeof(S.draft), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    const bool go = accent_button(tr("Envoyer", "Send"), ImVec2(100, 0));
    ImGui::EndDisabled();
    if ((enter || go) && S.draft[0] != '\0' && !cur->blocked) {
        std::string err;
        const bool ok = tl ? account::send(peer, S.draft, &err)
                           : social::send(cur->id, S.draft, &err);
        if (ok)
            S.draft[0] = '\0';
        else
            S.lastError = err;
    }
    // (Notifications relevees au niveau de la page : voir social_page.)
}

// Le signalement n'a pas d'API dans le SDK social : pas de bouton leurre.
// Pour signaler un utilisateur, passer par le client Discord lui-meme.

} // namespace

// Mise en page partagee des deux sources : liste a gauche, fil a droite.
void draw_split(const std::vector<social::Friend>& list) {
    // Bloc d'amis proportionnel a la page (36 %), borne : assez large
    // pour les longs pseudos, sans ecraser la conversation.
    float friendsW = ImGui::GetContentRegionAvail().x * 0.36f;
    if (friendsW < 300.0f) friendsW = 300.0f;
    if (friendsW > 440.0f) friendsW = 440.0f;
    ImGui::BeginChild("##friends", ImVec2(friendsW, 0),
                      ImGuiChildFlags_Borders);
    draw_friend_list(list);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##conv", ImVec2(0, 0), ImGuiChildFlags_Borders);
    draw_conversation(list);
    ImGui::EndChild();

    if (!S.lastError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", S.lastError.c_str());
        ImGui::PopStyleColor();
    }
}

void social_page() {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Amis"));
    if (fBig) ImGui::PopFont();

    // Source : Discord (SDK) ou compte maison (API). Meme mise en page,
    // donnees et actions routees par S.src. Le compteur signale les
    // demandes d'amis en attente cote maison.
    if (ImGui::SmallButton(tr("Discord"))) S.src = 0;
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Team Launcher"))) S.src = 1;
    {
        const int demandes =
            static_cast<int>(account::pending_in().size());
        if (demandes > 0) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::Text("(%d)", demandes);
            ImGui::PopStyleColor();
        }
    }
    // Notifications des deux backends, relevees ici (pas seulement dans
    // la conversation) : sinon, sans fil ouvert, on rate tout.
    if (std::string sendErr = social::take_send_error(); !sendErr.empty())
        S.lastError = sendErr;
    if (std::string accErr = account::take_send_error(); !accErr.empty())
        S.lastError = accErr;
    if (std::string callNote = social::take_call_notice(); !callNote.empty())
        S.lastError = callNote;
    if (std::string accCall = account::take_call_notice(); !accCall.empty())
        S.lastError = accCall;
    if (std::string note = account::take_notice(); !note.empty())
        S.lastError = note;
    if (std::string socNote = social::take_notice(); !socNote.empty())
        S.lastError = socNote;
    ImGui::Spacing();

    if (tl_tab()) {
        const account::Status ast = account::status();
        if (!ast.configured) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s", tr("Compte Team Launcher non configuré : renseigne "
                         "l'URL de l'API dans Paramètres > Intégrations.",
                         "Team Launcher account not configured: set the API "
                         "URL in Settings > Integrations."));
            ImGui::PopStyleColor();
            return;
        }
        if (!ast.loggedIn) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s", tr("Connecte ton compte dans Paramètres > "
                         "Intégrations.",
                         "Sign in to your account in Settings > "
                         "Integrations."));
            if (!ast.detail.empty()) {
                ImGui::Spacing();
                ImGui::TextWrapped("%s", ast.detail.c_str());
            }
            ImGui::PopStyleColor();
            return;
        }
        // Liste mappee au format d'affichage (ids prefixes "tl:").
        std::vector<social::Friend> list;
        for (const auto& f : account::friends()) {
            social::Friend m;
            m.id = "tl:" + f.name;
            m.name = f.name;
            m.presence = f.online ? social::Presence::Online
                                  : social::Presence::Offline;
            m.unread = f.unread;
            m.blocked = f.blocked;
            list.push_back(std::move(m));
        }
        draw_split(list);
        return;
    }

    const auto st = social::status();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s", tr("Amis, messages et appels passent par Discord : le launcher "
                 "n'héberge aucun serveur et ne conserve aucun message.",
                 "Friends, messages and calls go through Discord: the "
                 "launcher hosts no server and keeps no message."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (!st.ready) {
        draw_unavailable(st);
        return;
    }

    draw_split(social::friends());
}

} // namespace tl::ui

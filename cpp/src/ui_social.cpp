#include "ui_internal.hpp"

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
    std::string selected;      // ami courant
    char draft[512] = "";      // message en cours de saisie
    char addBuf[64] = "";      // pseudo a ajouter
    char searchBuf[64] = "";   // filtre de recherche local
    char reportBuf[200] = "";  // motif de signalement
    std::string reportTarget;  // ami vise par le signalement
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
        if (social::add_friend(S.addBuf, &err))
            S.addBuf[0] = '\0';
        else
            S.lastError = err;
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

    // --- en-tete : nom, vocal, blocage, signalement ---
    const social::CallInfo ci = social::call_info();
    const bool voiceOk = social::status().voiceAvailable;
    // Banniere d'appel entrant, quel que soit le fil affiche.
    if (ci.incoming) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(tr("Appel entrant de :", "Incoming call from:"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(ci.peerName.c_str());
        ImGui::SameLine();
        if (accent_button(tr("Décrocher", "Answer"), ImVec2(120, 0))) {
            std::string err;
            if (!social::call_accept(&err)) S.lastError = err;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Refuser", "Decline"))) {
            std::string err;
            if (!social::call_decline(&err)) S.lastError = err;
        }
        ImGui::Separator();
    }
    ImGui::TextUnformatted(cur->name.c_str());
    ImGui::SameLine();
    if (ci.idle) {
        // Aucun appel : proposer d'appeler ce correspondant.
        ImGui::BeginDisabled(!voiceOk || cur->blocked);
        if (ImGui::SmallButton(tr("Appel vocal", "Voice call"))) {
            std::string err;
            if (!social::call_start(cur->id, &err)) S.lastError = err;
        }
        ImGui::EndDisabled();
    } else if (ci.peerId == cur->id && ci.outgoing) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Appel en cours…", "Calling…"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Raccrocher", "Hang up"))) {
            std::string err;
            if (!social::call_hangup(&err)) S.lastError = err;
        }
    } else if (ci.peerId == cur->id && ci.active) {
        if (ImGui::SmallButton(ci.muted ? tr("Micro coupé", "Muted")
                                        : tr("Micro", "Mute"))) {
            std::string err;
            if (!social::call_set_muted(!ci.muted, &err)) S.lastError = err;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(ci.deaf ? tr("Son coupé", "Deafened")
                                       : tr("Son", "Deafen"))) {
            std::string err;
            if (!social::call_set_deaf(!ci.deaf, &err)) S.lastError = err;
        }
        ImGui::SameLine();
        const long long dur = ci.startedUnix > 0
                                  ? std::time(nullptr) - ci.startedUnix
                                  : 0;
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%02lld:%02lld",
                      dur / 60 < 0 ? 0 : dur / 60, dur % 60 < 0 ? 0 : dur % 60);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(buf);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Raccrocher", "Hang up"))) {
            std::string err;
            if (!social::call_hangup(&err)) S.lastError = err;
        }
    } else {
        // Appel avec un autre correspondant : le signaler sans proposer.
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("En appel avec :", "In a call with:"));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(ci.peerName.c_str());
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(cur->blocked ? tr("Débloquer", "Unblock")
                                        : tr("Bloquer", "Block"))) {
        std::string err;
        if (!social::set_blocked(cur->id, !cur->blocked, &err))
            S.lastError = err;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Signaler", "Report")))
        S.reportTarget = cur->id;
    ImGui::Separator();

    // --- fil ---
    const float composerH = 78.0f;
    ImGui::BeginChild("##thread",
                      ImVec2(0, ImGui::GetContentRegionAvail().y - composerH));
    const auto msgs = social::conversation(cur->id);
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
        if (social::send(cur->id, S.draft, &err))
            S.draft[0] = '\0';
        else
            S.lastError = err;
    }
    // Echec d'envoi asynchrone (reponse du SDK apres coup) : affiche une
    // fois, puis oublie. Idem pour les evenements d'appel.
    if (std::string sendErr = social::take_send_error(); !sendErr.empty())
        S.lastError = sendErr;
    if (std::string callNote = social::take_call_notice(); !callNote.empty())
        S.lastError = callNote;
}

void draw_report_modal() {
    if (S.reportTarget.empty()) return;
    ImGui::OpenPopup("###socreport");
    const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f,
                   ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 220), ImGuiCond_Appearing);
    const std::string title =
        std::string(tr("Signaler", "Report")) + "###socreport";
    bool open = true;
    if (ImGui::BeginPopupModal(title.c_str(), &open,
                               ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("Le signalement est transmis à Discord, qui applique sa "
                     "propre modération. Le launcher ne conserve rien.",
                     "The report goes to Discord, which applies its own "
                     "moderation. The launcher keeps nothing."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##reason", tr("Motif", "Reason"),
                                 S.reportBuf, sizeof(S.reportBuf));
        ImGui::PopStyleColor();
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52.0f);
        if (ImGui::Button(tr("Annuler"), ImVec2(150, 34))) {
            S.reportTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(S.reportBuf[0] == '\0');
        if (danger_button(tr("Signaler", "Report"), ImVec2(150, 34))) {
            std::string err;
            if (!social::report(S.reportTarget, S.reportBuf, &err))
                S.lastError = err;
            S.reportBuf[0] = '\0';
            S.reportTarget.clear();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
    if (!open) S.reportTarget.clear();
}

} // namespace

void social_page() {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Amis"));
    if (fBig) ImGui::PopFont();

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

    const auto list = social::friends();
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

    draw_report_modal();

    if (!S.lastError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", S.lastError.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace tl::ui

#include "ui_internal.hpp"

#include "icons.hpp"
#include "social.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
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
    char reportBuf[200] = "";  // motif de signalement
    std::string reportTarget;  // ami vise par le signalement
    std::string lastError;
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
    for (const auto& f : list) {
        ImGui::PushID(f.id.c_str());
        const bool sel = f.id == S.selected;
        if (ImGui::Selectable("##f", sel, 0, ImVec2(0, 38.0f)))
            S.selected = f.id;
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 4));
        ImGui::PushStyleColor(ImGuiCol_Text, f.blocked ? kDim : kText);
        ImGui::TextUnformatted(f.name.c_str());
        ImGui::PopStyleColor();
        ImGui::SetCursorScreenPos(ImVec2(p.x + 8, p.y + 20));
        ImGui::PushStyleColor(ImGuiCol_Text, presence_color(f.presence));
        ImGui::TextUnformatted(f.blocked ? tr("bloqué", "blocked")
                                         : tr(presence_label(f.presence)));
        ImGui::PopStyleColor();
        if (f.unread > 0) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::Text("(%d)", f.unread);
            ImGui::PopStyleColor();
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 38.0f));
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

    // --- en-tete : nom, appel vocal (a venir), blocage, signalement ---
    ImGui::TextUnformatted(cur->name.c_str());
    ImGui::SameLine();
    // Le vocal arrive dans une phase separee : le bouton est la, mais
    // desactive et annonce comme tel. Mieux vaut une promesse lisible
    // qu'un bouton absent qu'on croit oublie.
    ImGui::BeginDisabled(true);
    ImGui::SmallButton(tr("Appel vocal", "Voice call"));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr("(à venir)", "(coming soon)"));
    ImGui::PopStyleColor();
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
    ImGui::BeginChild("##friends", ImVec2(240, 0), ImGuiChildFlags_Borders);
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

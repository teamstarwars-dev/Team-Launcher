#include "ui_internal.hpp"

#include "ms_auth.hpp"

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Compte (fidele AccountPage C#) : profil, mode d'auth, pseudo.
// Auth Microsoft branchee (module 4) ; avatar réseau toujours a porter.
// ---------------------------------------------------------------------------

void account_page() {
    auto& st = DataStore::settings;

    static char pseudoBuf[64];
    static bool init = false;
    if (!init) {
        std::snprintf(pseudoBuf, sizeof(pseudoBuf), "%s", st.playerName.c_str());
        init = true;
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Compte"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    const std::string name = st.playerName.empty() ? "Joueur" : st.playerName;

    // ---- Carte profil : avatar cercle accent + initiale (C# 64x64 radius 32) ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##profile", ImVec2(0, 116), ImGuiChildFlags_Borders);
    {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 c(p.x + 40.0f, p.y + 40.0f);
        dl->AddCircleFilled(c, 32.0f,
                            ImGui::ColorConvertFloat4ToU32(kAccent), 48);
        char initial[2] = {name.empty() ? '?'
                                        : (char)toupper((unsigned char)name[0]),
                           0};
        const ImVec2 ts = ImGui::CalcTextSize(initial);
        dl->AddText(ImVec2(c.x - ts.x / 2, c.y - ts.y / 2), IM_COL32(255, 255, 255, 255),
                    initial);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 92.0f, p.y + 24.0f));
        ImGui::TextUnformatted(name.c_str());
        ImGui::SetCursorScreenPos(ImVec2(p.x + 92.0f, p.y + 56.0f));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(st.accountMode == "microsoft" && auth::has_session()
                                   ? tr("Compte Microsoft")
                                   : tr("Joueur Minecraft"));
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // ---- Mode d'authentification (C# radio GroupName AccountMode) ----
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr("Mode d'authentification"));
    ImGui::PopStyleColor();
    const bool isMs = st.accountMode == "microsoft";
    if (ImGui::RadioButton(tr("Compte Microsoft"), isMs)) {
        st.accountMode = "microsoft";
        DataStore::save();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Mode Hors-ligne"), !isMs)) {
        st.accountMode = "offline";
        DataStore::save();
    }
    ImGui::Spacing();

    // ---- Panneau de statut (C# _statusPanel) ----
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##authstatus", ImVec2(0, isMs ? 120.0f : 92.0f),
                      ImGuiChildFlags_Borders);
    if (!isMs) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Choisir un pseudo"));
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(320.0f);
        ImGui::InputTextWithHint("##pseudo", tr("Pseudo..."), pseudoBuf,
                                 sizeof(pseudoBuf));
        ImGui::SameLine();
        if (accent_button(tr("Valider"), ImVec2(80, 0))) {
            st.playerName = trimmed(pseudoBuf);
            if (st.playerName.empty()) st.playerName = "Joueur";
            DataStore::save();
            notify_toast(tr("Pseudo mis à jour"),
                         "« " + st.playerName + " »" +
                             tr(" sera utilisé au lancement.",
                                " will be used when launching."));
        }
    } else {
        const auto session = auth::get_session();
        const bool busy = auth::get_state() != auth::AuthState::Idle;

        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        const std::string signedIn =
            session ? tr("Connecté en tant que ", "Signed in as ") + session->name +
                          tr(" (compte Microsoft)", " (Microsoft account)")
                    : std::string();
        ImGui::TextUnformatted(
            session ? signedIn.c_str()
                    : tr("Connexion officielle par code d'appareil : le launcher "
                         "ouvre microsoft.com/link, tu entres le code affiché."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        ImGui::BeginDisabled(busy);
        if (accent_button(session ? tr("Changer de compte") : tr("Se connecter avec Microsoft"),
                          ImVec2(300, 34)))
            auth::login_start(/*force=*/session.has_value());
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Se déconnecter"), ImVec2(150, 34))) {
            auth::logout();
            st.accountMode = "offline";
            st.playerName = "Joueur";
            std::snprintf(pseudoBuf, sizeof(pseudoBuf), "Joueur");
            DataStore::save();
            notify_toast(tr("Déconnecté"),
                         tr("Jeton supprimé de ce PC. Retour en mode hors ligne."));
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

} // namespace tl::ui

#include "ui_internal.hpp"

#include "ms_auth.hpp"

// ---------------------------------------------------------------------------
// Assistant de premier lancement (portage d'OnboardingDialog.cs).
//
// Le C# instanciait le dialogue avec SkipImport = true (Program.cs) : l'etape
// « import des instances existantes » n'etait donc jamais atteinte en usage
// reel. On porte les deux etapes qui comptent — Bienvenue et Compte — et la
// detection de .minecraft reste a faire (elle vit dans la page Instances).
//
// Declenchement, fidele au C# : !onboardingDone OU accountMode vide.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

int s_step = 0;
bool s_opened = false;
bool s_waitingLogin = false;
char s_pseudo[64] = "";
bool s_pseudoError = false;

void finish() {
    DataStore::settings.onboardingDone = true;
    DataStore::save();
    ImGui::CloseCurrentPopup();
}

void step_welcome(float w) {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Bienvenue dans ton nouveau launcher !"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(w);
    ImGui::TextUnformatted(
        tr("Léger, rapide et sans pub : tes instances, tes mods, tes serveurs.\n\n"
           "En deux minutes :\n"
           "   1. Connecte ton compte Microsoft (ou joue hors-ligne)\n"
           "   2. Crée ou importe tes instances Minecraft\n"
           "   3. Installe des mods depuis Modrinth et CurseForge en un clic\n\n"
           "Tout est prêt ? C'est parti !"));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 60.0f);
    if (accent_button(tr("Commencer"), ImVec2(200, 40))) s_step = 1;
}

void step_account(float w) {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Connecte-toi pour jouer"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::Spacing();

    const float bw = (std::min)(380.0f, w);
    const float cx = (w - bw) * 0.5f;

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
    if (accent_button(tr("Se connecter avec Microsoft"), ImVec2(bw, 52))) {
        // Fidele au C# : le mode est pose AVANT la connexion.
        DataStore::settings.accountMode = "microsoft";
        DataStore::save();
        s_waitingLogin = true;
        // La modale d'auth prend le relais : celle-ci s'efface le temps de la
        // connexion (deux modales ImGui empilees se genent).
        auth::login_start(/*force=*/false);
    }

    ImGui::Spacing();
    ImGui::Spacing();
    {
        const char* sep = tr("ou", "or");
        const float sw = ImGui::CalcTextSize(sep).x;
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (w - sw) * 0.5f);
        ImGui::TextUnformatted(sep);
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
    ImGui::Spacing();

    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
    ImGui::SetNextItemWidth(bw);
    // Le fond de la modale est deja kCard : un champ en kCard serait invisible.
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, kBg);
    const bool enter = ImGui::InputTextWithHint(
        "##obpseudo", tr("Ton pseudo pour le mode hors-ligne"), s_pseudo,
        sizeof(s_pseudo), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(2);

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
    const bool offline = ImGui::Button(tr("Continuer hors-ligne"), ImVec2(bw, 44));

    if (offline || enter) {
        const std::string name = trimmed(s_pseudo);
        if (name.empty()) {
            s_pseudoError = true;
        } else {
            s_pseudoError = false;
            DataStore::settings.accountMode = "offline";
            DataStore::settings.playerName = name;
            finish();
        }
    }
    if (s_pseudoError) {
        ImGui::Spacing();
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + cx);
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(tr("Entre un pseudo pour le mode hors-ligne."));
        ImGui::PopStyleColor();
    }
}

} // namespace

bool onboarding_needed() {
    const auto& s = DataStore::settings;
    return !s.onboardingDone || s.accountMode.empty();
}

void onboarding_frame() {
    if (!onboarding_needed()) {
        s_opened = false;
        return;
    }

    // Connexion Microsoft lancee depuis l'assistant : on laisse la modale
    // d'auth travailler seule, puis on conclut selon le resultat.
    if (s_waitingLogin) {
        if (auth::get_state() != auth::AuthState::Idle) return; // auth en cours
        s_waitingLogin = false;
        if (auth::has_session()) {
            // auth_sync() a deja pose playerName et le toast de bienvenue.
            DataStore::settings.onboardingDone = true;
            DataStore::save();
            s_opened = false;
            return;
        }
        // Echec ou annulation : on reste sur l'etape Compte.
    }

    if (!s_opened) {
        ImGui::OpenPopup("###onboarding");
        s_opened = true;
        // TL_AUTO_ONBOARD_STEP=0|1 : ouvre directement une etape (test).
        if (const char* st = std::getenv("TL_AUTO_ONBOARD_STEP"))
            s_step = std::atoi(st);
    }

    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(ds.x * 0.5f, ds.y * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560, 400), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, kCard);
    const std::string title = std::string(tr("Team Launcher")) + "###onboarding";
    // Pas de croix : l'assistant doit aboutir (le C# n'avait pas de sortie non
    // plus tant qu'aucun mode de compte n'etait choisi).
    if (ImGui::BeginPopupModal(title.c_str(), nullptr,
                               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoScrollbar)) {
        const float w = ImGui::GetContentRegionAvail().x;
        if (s_step == 0)
            step_welcome(w);
        else
            step_account(w);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

} // namespace tl::ui

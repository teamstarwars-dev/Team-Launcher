#include "ui_internal.hpp"

#include "ms_auth.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <utility>

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

// --- Visite guidee ---------------------------------------------------------
// Chaque etape pointe un bouton de la barre laterale (par son index de page)
// et l'explique. Les rectangles ecran des boutons sont releves chaque frame
// par onboarding_record_nav() depuis la nav de ui.cpp.
struct TourStep {
    int page;
    const char* title;
    const char* text;
};
const TourStep kTour[] = {
    {1, "Tes instances",
     "Chaque instance est une version de Minecraft avec ses mods et ses "
     "réglages. Clique « Créer » pour en faire une, ou importe un modpack."},
    {9, "Explorer les mods",
     "Cherche et installe des mods depuis Modrinth et CurseForge en un clic, "
     "directement dans l'instance choisie."},
    {2, "Jouer",
     "Lance l'instance sélectionnée. Le launcher télécharge ce qu'il faut et "
     "démarre le jeu."},
    {4, "Skins",
     "Change ton skin Minecraft et gère tes favoris."},
    {18, "Nova Skin",
     "Crée ou retouche ton skin dans l'éditeur 3D, directement intégré."},
    {15, "Téléchargements",
     "Suis l'avancement de tes téléchargements et installations."},
    {7, "Ton compte",
     "Gère ta connexion Microsoft, ton profil et le mode hors-ligne."},
    {8, "Paramètres",
     "RAM allouée, version de Java, thèmes, intégrations… tout se règle ici."},
};
constexpr int kTourCount = static_cast<int>(sizeof(kTour) / sizeof(kTour[0]));

bool s_tourActive = false;
int s_tourStep = 0;
std::map<int, std::pair<ImVec2, ImVec2>> s_navRects;

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
            DataStore::save();
            s_step = 2; // -> proposition du guide
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

void step_guide(float w) {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Tout est prêt !"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::PushTextWrapPos(w);
    ImGui::TextUnformatted(
        tr("Première fois ici ? Je peux te faire une visite rapide : je te "
           "présente chaque bouton en quelques secondes. Tu peux arrêter quand "
           "tu veux."));
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 64.0f);
    if (accent_button(tr("Commencer le guide"), ImVec2(240, 44))) {
        finish();      // ferme l'assistant (onboardingDone = true)
        start_guide(); // lance la visite sur l'interface reelle
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Passer"), ImVec2(120, 44))) finish();
}

} // namespace

void start_guide() {
    s_tourActive = true;
    s_tourStep = 0;
}

void onboarding_record_nav(int page) {
    s_navRects[page] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
}

void onboarding_tour_frame() {
    if (!s_tourActive) return;
    if (s_tourStep < 0 || s_tourStep >= kTourCount) {
        s_tourActive = false;
        return;
    }
    const TourStep& step = kTour[s_tourStep];

    // Surbrillance du bouton cible (liste de dessin de premier plan : par-dessus
    // tout le reste). Sans dim plein ecran, pour eviter les soucis de calque.
    ImVec2 mn(0, 0), mx(0, 0);
    bool haveRect = false;
    if (auto it = s_navRects.find(step.page); it != s_navRects.end()) {
        mn = it->second.first;
        mx = it->second.second;
        haveRect = true;
    }
    ImDrawList* fg = ImGui::GetForegroundDrawList();
    if (haveRect) {
        fg->AddRectFilled(mn, mx, IM_COL32(255, 170, 60, 40), 6.0f);
        fg->AddRect(ImVec2(mn.x - 2, mn.y - 2), ImVec2(mx.x + 2, mx.y + 2),
                    IM_COL32(255, 170, 60, 255), 6.0f, 0, 2.5f);
    }

    // Bulle de texte, posee a droite du bouton (ou centree si pas de rect).
    const ImVec2 ds = ImGui::GetIO().DisplaySize;
    const float boxW = 340.0f;
    ImVec2 pos = haveRect ? ImVec2(mx.x + 16.0f, mn.y)
                          : ImVec2((ds.x - boxW) * 0.5f, ds.y * 0.35f);
    if (pos.x + boxW > ds.x - 12.0f) pos.x = ds.x - boxW - 12.0f;
    if (pos.x < 12.0f) pos.x = 12.0f;

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(boxW, 0));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, kCard);
    ImGui::Begin("##tour", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_AlwaysAutoResize);

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr(step.title));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();
    ImGui::PushTextWrapPos(boxW - 20.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr(step.text));
    ImGui::PopStyleColor();
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    ImGui::Spacing();
    char prog[24];
    std::snprintf(prog, sizeof(prog), "%d / %d", s_tourStep + 1, kTourCount);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(prog);
    ImGui::PopStyleColor();
    ImGui::SameLine();

    const bool last = (s_tourStep == kTourCount - 1);
    // Boutons alignes a droite.
    const float btnW = last ? 110.0f : 90.0f;
    const float skipW = 80.0f;
    float avail = ImGui::GetContentRegionAvail().x;
    ImGui::SameLine(0.0f, avail - btnW - skipW - 8.0f > 0 ? avail - btnW - skipW - 8.0f : 8.0f);
    if (ImGui::Button(tr("Passer"), ImVec2(skipW, 0))) s_tourActive = false;
    ImGui::SameLine();
    if (accent_button(last ? tr("Terminer") : tr("Suivant"), ImVec2(btnW, 0))) {
        if (last)
            s_tourActive = false;
        else
            ++s_tourStep;
    }

    ImGui::End();
    ImGui::PopStyleColor();
}

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
        // auth_sync() a deja pose playerName et le toast de bienvenue. On ne
        // CLOT pas encore l'assistant : on propose d'abord la visite guidee.
        if (auth::has_session()) s_step = 2;
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
        else if (s_step == 2)
            step_guide(w);
        else
            step_account(w);
        ImGui::EndPopup();
    }
    ImGui::PopStyleColor();
}

} // namespace tl::ui

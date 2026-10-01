#include "ui_internal.hpp"

#include "content_install.hpp"
#include "icons.hpp"
#include "modcheck.hpp"

#include <thread>

// ---------------------------------------------------------------------------
// Phase 5 — affichage de la compatibilite des mods.
//
// Un seul rendu, deux points d'entree : le panneau de l'onglet Mods de la
// page detail, et la modale qui barre le lancement quand un probleme est
// bloquant. Les tenir separes aurait garanti qu'ils divergent.
//
// L'analyse ouvre une archive par mod : cent mods, cent lectures de zip.
// C'est trop pour le fil de l'interface, d'ou le travailleur — et le cache
// par instance, qui evite de tout relire a chaque frame.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct Scan {
    std::mutex m;
    std::thread th;
    bool busy = false;
    bool done = false;         // rapport exploitable
    std::string instId;        // instance du rapport courant
    std::string pendingId;     // instance en cours d'analyse
    modcheck::Report rep;
};
Scan s;

void reap() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (s.busy || !s.th.joinable()) return;
        th = std::move(s.th);
    }
    th.join();
}

void start(const std::string& instId, const std::filesystem::path& modsDir,
           const std::string& loader, const std::string& mcVersion) {
    {
        std::lock_guard<std::mutex> lk(s.m);
        if (s.busy) return;
        s.busy = true;
        s.done = false;
        s.pendingId = instId;
    }
    reap();
    std::lock_guard<std::mutex> lk(s.m);
    s.th = std::thread([instId, modsDir, loader, mcVersion] {
        auto rep = modcheck::scan(modsDir, loader, mcVersion);
        std::lock_guard<std::mutex> lk2(s.m);
        s.busy = false;
        s.done = true;
        s.instId = instId;
        s.rep = std::move(rep);
    });
}

// Version de Minecraft a comparer. « latest » n'est pas une version : la
// comparer aux contraintes des mods declarerait tout incompatible. On rend
// alors une chaine vide, que modcheck comprend comme « ne verifie pas ».
std::string effective_mc(const nlohmann::json& inst) {
    const std::string v = inst.value("McVersion", "");
    if (v.empty() || v == "latest" || v == "release" || v == "snapshot")
        return {};
    return v;
}

ImVec4 color_of(modcheck::Severity sev) {
    switch (sev) {
    case modcheck::Severity::Error: return kDanger;
    case modcheck::Severity::Warning: return hex(0xE0A030);
    default: return kDim;
    }
}

// Etat de la modale de pre-lancement.
bool gateOpen = false;
bool gateRequest = false;
modcheck::Report gateRep;
std::string gateInstId;

} // namespace

void modcheck_report_view(const modcheck::Report& rep) {
    if (rep.issues.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0x4ADE80));
        ImGui::Text(tr("Aucun problème détecté sur %d mod(s).",
                       "No problem found across %d mod(s)."),
                    rep.analysed);
        ImGui::PopStyleColor();
    }
    for (const auto& i : rep.issues) {
        ImGui::PushStyleColor(ImGuiCol_Text, color_of(i.sev));
        ImGui::TextUnformatted(modcheck::severity_label(i.sev));
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::TextUnformatted(i.title.c_str());
        if (!i.file.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::SameLine();
            ImGui::Text("— %s", i.file.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", i.detail.c_str());
        ImGui::PopStyleColor();
        ImGui::Spacing();
    }

    // Suggestions : elles sortent des dependances FACULTATIVES declarees
    // par les mods deja installes. C'est la seule source honnete dont on
    // dispose — un catalogue de « mods qui vont bien ensemble » serait
    // inventé.
    if (!rep.suggestions.empty()) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Compléments possibles",
                                  "Possible companions"));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("Vos mods savent tirer parti de ceux-ci, sans les "
                     "exiger. Cliquez pour les chercher.",
                     "Your mods can make use of these, without requiring "
                     "them. Click to search for one."));
        ImGui::PopStyleColor();
        for (const auto& sug : rep.suggestions) {
            ImGui::PushID(sug.c_str());
            if (ImGui::SmallButton(sug.c_str())) {
                // Renvoie vers la page Exploration avec le terme pret.
                g.page = 9;
                explore_search(sug);
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
        ImGui::NewLine();
    }
}

void modcheck_panel(const nlohmann::json& inst) {
    reap();
    const std::string id = inst.value("Id", "");
    const std::filesystem::path modsDir = DataStore::instancesRoot() / id / "mods";

    bool busy = false, done = false;
    std::string have;
    modcheck::Report rep;
    {
        std::lock_guard<std::mutex> lk(s.m);
        busy = s.busy;
        done = s.done;
        have = s.instId;
        if (done) rep = s.rep;
    }

    // Analyse automatique a l'ouverture, puis sur demande : l'utilisateur
    // n'a pas a cliquer pour apprendre que son instance ne demarrera pas.
    if (!busy && (!done || have != id))
        start(id, modsDir, inst.value("Loader", "Vanilla"), effective_mc(inst));

    ImGui::Spacing();
    if (busy || have != id) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Analyse de la compatibilité...",
                                  "Checking compatibility..."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::Separator();
        return;
    }

    // Bandeau de resume, couleur du pire constat.
    const ImVec4 col = rep.errors    ? kDanger
                       : rep.warnings ? hex(0xE0A030)
                                      : hex(0x4ADE80);
    ImGui::PushStyleColor(ImGuiCol_Text, col);
    if (rep.errors)
        ImGui::Text(tr("%d problème(s) bloquant(s), %d avertissement(s)",
                       "%d blocking problem(s), %d warning(s)"),
                    rep.errors, rep.warnings);
    else if (rep.warnings)
        ImGui::Text(tr("%d avertissement(s)", "%d warning(s)"), rep.warnings);
    else
        ImGui::Text(tr("Compatibilité : rien à signaler (%d mod(s))",
                       "Compatibility: nothing to report (%d mod(s))"),
                    rep.analysed);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Réanalyser", "Re-check")))
        start(id, modsDir, inst.value("Loader", "Vanilla"), effective_mc(inst));
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Comparer avec...", "Compare with...")))
        compare_open(id);
    if (rep.skipped > 0) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text(tr("%d mod(s) désactivé(s), non analysé(s).",
                       "%d disabled mod(s), not checked."),
                    rep.skipped);
        ImGui::PopStyleColor();
    }

    // Detail replie par defaut. Ouvert, il faisait plusieurs ecrans de
    // haut et repoussait hors de vue tout ce qui suit dans l'onglet — les
    // mises a jour, puis la liste des mods elle-meme. Le resume au-dessus
    // suffit a savoir s'il faut ouvrir, et le barrage avant lancement
    // deroule tout, lui, au moment ou cela compte.
    if (!rep.issues.empty() || !rep.suggestions.empty()) {
        const int n = static_cast<int>(rep.issues.size());
        if (ImGui::TreeNode("##compatdet", "%s (%d)", tr("Détail", "Details"),
                            n)) {
            modcheck_report_view(rep);
            ImGui::TreePop();
        }
    }
    ImGui::Spacing();
    ImGui::Separator();
}

void modcheck_invalidate() {
    std::lock_guard<std::mutex> lk(s.m);
    // On n'annule pas une analyse en cours : elle se termine et son
    // resultat sera juste remplace au prochain passage.
    s.done = false;
    s.instId.clear();
}

void modcheck_open_gate(const std::string& instId, const modcheck::Report& rep) {
    gateInstId = instId;
    gateRep = rep;
    gateRequest = true;
}

void modcheck_gate_modal() {
    if (gateRequest) {
        gateRequest = false;
        gateOpen = true;
        ImGui::OpenPopup("##compatgate");
    }
    if (!gateOpen) return;
    ImGui::SetNextWindowSize(ImVec2(660, 480), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##compatgate", nullptr,
                                ImGuiWindowFlags_NoSavedSettings))
        return;

    if (fBig) ImGui::PushFont(fBig);
    ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
    ImGui::TextUnformatted(tr("Le jeu ne démarrera probablement pas",
                              "The game will probably not start"));
    ImGui::PopStyleColor();
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Ces problèmes viennent de ce que les mods déclarent eux-mêmes. "
           "Sans cette vérification, Minecraft se serait fermé en une "
           "seconde avec une pile d'erreurs.",
           "These problems come from what the mods declare themselves. "
           "Without this check, Minecraft would have closed after a second "
           "with a stack trace."));
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::BeginChild("##gatebody", ImVec2(0, -46));
    modcheck_report_view(gateRep);
    ImGui::EndChild();

    if (accent_button(tr("Corriger les mods", "Fix the mods"),
                      ImVec2(200, 34))) {
        gateOpen = false;
        ImGui::CloseCurrentPopup();
        open_instance_detail(gateInstId, /*tab=*/1);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Annuler"), ImVec2(130, 34))) {
        gateOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    // Dernier recours, volontairement a droite et sans accent : notre
    // analyse peut se tromper (un mod qui declare mal ses contraintes),
    // et l'utilisateur doit garder la main sur sa machine.
    if (danger_button(tr("Lancer quand même", "Launch anyway"),
                      ImVec2(190, 34))) {
        gateOpen = false;
        ImGui::CloseCurrentPopup();
        g.compatSkipId = gateInstId;
        start_worker();
    }
    ImGui::EndPopup();
}

void modcheck_stop() {
    reap();
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(s.m);
        th = std::move(s.th);
    }
    // Lecture de fichiers locaux : quelques centaines de millisecondes au
    // pire, rien a annuler.
    if (th.joinable()) th.join();
}

} // namespace tl::ui

#include "ui_internal.hpp"

#include "downloads.hpp"
#include "icons.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <thread>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Gestionnaire de telechargements (page 15).
//
// Deux onglets avec compteurs, recherche, tri, selection multiple pour les
// actions groupees, et un etat vide explicite. Les donnees viennent de
// `downloads::snapshot()` ; cette page ne telecharge rien elle-meme.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct DlState {
    char query[96] = "";
    int sort = 0;          // 0 recent, 1 nom, 2 progression, 3 taille
    int tab = 0;           // 0 en cours, 1 termines
    std::set<int> checked; // selection, par identifiant
};
DlState D;

std::string human_size(long long n) {
    if (n < 0) return "?";
    const char* u[] = {"o", "Ko", "Mo", "Go"};
    double v = static_cast<double>(n);
    int i = 0;
    while (v >= 1024.0 && i < 3) {
        v /= 1024.0;
        ++i;
    }
    char buf[48];
    std::snprintf(buf, sizeof(buf), i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
    return buf;
}

std::string human_speed(double bps) {
    if (bps <= 1.0) return {};
    return human_size(static_cast<long long>(bps)) + "/s";
}

// Temps restant estime. Sans taille totale ou sans vitesse, on n'invente
// rien : mieux vaut ne rien afficher qu'un chiffre faux.
std::string eta(const downloads::Item& x) {
    if (x.total <= 0 || x.speedBps <= 1.0 || x.state != downloads::State::Running)
        return {};
    const double left = static_cast<double>(x.total - x.done) / x.speedBps;
    if (left < 0 || left > 86400) return {};
    const int s = static_cast<int>(left);
    char buf[32];
    if (s >= 60)
        std::snprintf(buf, sizeof(buf), "%d min %02d s", s / 60, s % 60);
    else
        std::snprintf(buf, sizeof(buf), "%d s", s);
    return buf;
}

const char* state_label(downloads::State s) {
    switch (s) {
        case downloads::State::Queued: return "En attente";
        case downloads::State::Running: return "En cours";
        case downloads::State::Done: return "Terminé";
        case downloads::State::Failed: return "Échec";
        case downloads::State::Cancelled: return "Annulé";
    }
    return "";
}

ImVec4 state_color(downloads::State s) {
    switch (s) {
        case downloads::State::Done: return ImVec4(0.31f, 0.78f, 0.31f, 1.0f);
        case downloads::State::Failed: return kDanger;
        case downloads::State::Cancelled: return kDim;
        default: return kAccent;
    }
}

bool matches(const downloads::Item& x, const std::string& q) {
    if (q.empty()) return true;
    auto low = [](std::string s) {
        for (char& c : s)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string n = low(q);
    return low(x.label).find(n) != std::string::npos ||
           low(x.dest.filename().string()).find(n) != std::string::npos ||
           low(x.url).find(n) != std::string::npos;
}

void sort_items(std::vector<downloads::Item>& v, int mode) {
    switch (mode) {
        case 1:
            // Comparaison insensible a la casse sans _stricmp, qui n'existe
            // pas sous Linux.
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                return std::lexicographical_compare(
                    a.label.begin(), a.label.end(), b.label.begin(),
                    b.label.end(), [](unsigned char x, unsigned char y) {
                        return std::tolower(x) < std::tolower(y);
                    });
            });
            break;
        case 2:
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                return a.progress() > b.progress();
            });
            break;
        case 3:
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) {
                return a.total > b.total;
            });
            break;
        default: break; // snapshot() rend deja du plus recent au plus ancien
    }
}

// Etat vide : une icone et une phrase qui dit quoi faire, plutot qu'un
// panneau blanc qui laisse croire a un bug.
void empty_state(bool activeTab) {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float cy = ImGui::GetCursorScreenPos().y + avail.y * 0.32f;
    const float cx = ImGui::GetCursorScreenPos().x + avail.x * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    icons::draw(dl, activeTab ? icons::Id::Download : icons::Id::Check,
                ImVec2(cx - 24, cy), 48.0f,
                ImGui::ColorConvertFloat4ToU32(kDim), 2.5f);

    const char* msg =
        activeTab ? tr("Aucun téléchargement en cours.",
                       "No download in progress.")
                  : tr("Aucun téléchargement terminé.", "No finished download.");
    const char* hint =
        activeTab
            ? tr("Les mods, modpacks et versions installés depuis "
                 "Exploration apparaîtront ici.",
                 "Mods, modpacks and versions installed from Explore will "
                 "show up here.")
            : tr("Les téléchargements terminés sont conservés jusqu'à ce que "
                 "tu les effaces.",
                 "Finished downloads are kept until you clear them.");

    ImGui::SetCursorScreenPos(ImVec2(cx - ImGui::CalcTextSize(msg).x * 0.5f,
                                     cy + 64));
    ImGui::TextUnformatted(msg);
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::SetCursorScreenPos(ImVec2(cx - ImGui::CalcTextSize(hint).x * 0.5f,
                                     cy + 88));
    ImGui::TextUnformatted(hint);
    ImGui::PopStyleColor();
}

void draw_row(const downloads::Item& x) {
    ImGui::PushID(x.id);

    bool sel = D.checked.count(x.id) != 0;
    if (ImGui::Checkbox("##sel", &sel)) {
        if (sel)
            D.checked.insert(x.id);
        else
            D.checked.erase(x.id);
    }
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::TextUnformatted(x.label.c_str());

    // Barre de progression : indeterminee quand la taille est inconnue.
    const double p = x.progress();
    std::string overlay;
    if (x.state == downloads::State::Running || x.state == downloads::State::Queued) {
        overlay = human_size(x.done);
        if (x.total > 0) overlay += " / " + human_size(x.total);
        const std::string sp = human_speed(x.speedBps);
        if (!sp.empty()) overlay += "  ·  " + sp;
        const std::string e = eta(x);
        if (!e.empty()) overlay += "  ·  " + e;
    } else {
        overlay = state_label(x.state);
        if (x.total > 0) overlay += "  ·  " + human_size(x.total);
    }
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, state_color(x.state));
    // Largeur = reste MOINS la place du bouton d action : avec -1 la barre
    // prenait toute la ligne et poussait « Annuler » hors de la vue.
    ImGui::ProgressBar(p < 0 ? 0.0f : static_cast<float>(p),
                       ImVec2(-150.0f, 18.0f), overlay.c_str());
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(x.dest.string().c_str());
    ImGui::PopStyleColor();
    if (!x.error.empty() && x.state == downloads::State::Failed) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", x.error.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    if (x.active()) {
        if (ImGui::SmallButton(tr("Annuler"))) downloads::cancel(x.id);
    } else if (x.state != downloads::State::Done) {
        if (ImGui::SmallButton(tr("Relancer", "Retry"))) downloads::retry(x.id);
    } else {
        if (ImGui::SmallButton(tr("Ouvrir le dossier", "Open folder")))
            open_in_explorer(x.dest.parent_path());
    }

    ImGui::Separator();
    ImGui::PopID();
}

} // namespace

void downloads_page() {
    // TL_AUTO_DL=<n> : remplit la file de n entrees factices pour verifier
    // le rendu des lignes (progression, vitesse, echec, annulation) sans
    // dependre d'un telechargement reel. Sans la variable, rien n'est
    // enfile : ce crochet ne change pas le comportement du launcher.
    static bool dlAuto = false;
    if (!dlAuto) {
        dlAuto = true;
        if (const char* n = std::getenv("TL_AUTO_DL")) {
            const int count = std::max(1, std::atoi(n));
            downloads::set_fetcher(
                [](const std::string&, const std::filesystem::path& d,
                   const downloads::ProgressFn& p, const std::atomic<bool>& c,
                   std::string* err) {
                    const long long total = 4LL * 1024 * 1024;
                    for (long long i = 0; i <= total; i += 64 * 1024) {
                        if (c.load()) return false;
                        if (p) p(i, total);
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(90));
                    }
                    // Une entree sur quatre echoue : l'etat « Echec » et le
                    // bouton « Relancer » doivent etre visibles aussi.
                    if (d.filename().string().find("3") != std::string::npos) {
                        if (err) *err = "Hote injoignable (démonstration).";
                        return false;
                    }
                    return true;
                });
            for (int i = 0; i < count; ++i)
                downloads::enqueue("Mod de démonstration " + std::to_string(i),
                                   "https://exemple.test/mod" +
                                       std::to_string(i) + ".jar",
                                   std::filesystem::temp_directory_path() /
                                       ("demo" + std::to_string(i) + ".jar"));
        }
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Téléchargements"));
    if (fBig) ImGui::PopFont();

    auto all = downloads::snapshot();
    std::size_t nActive = 0, nDone = 0;
    for (const auto& x : all) (x.active() ? nActive : nDone)++;

    // --- barre d'outils ---
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(280.0f);
    ImGui::InputTextWithHint("##dlq", tr("Rechercher…", "Search…"), D.query,
                             sizeof(D.query));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(190.0f);
    const char* sorts[] = {"Plus récents", "Nom", "Progression", "Taille"};
    ImGui::Combo("##dlsort", &D.sort, sorts, 4);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text(tr("%d simultanés", "%d at a time"), downloads::limit());
    ImGui::PopStyleColor();

    ImGui::Spacing();
    if (ImGui::BeginTabBar("##dltabs")) {
        char t0[64], t1[64];
        std::snprintf(t0, sizeof(t0), "%s (%zu)###dl0",
                      tr("En cours", "In progress"), nActive);
        std::snprintf(t1, sizeof(t1), "%s (%zu)###dl1",
                      tr("Terminés", "Finished"), nDone);
        if (ImGui::BeginTabItem(t0)) {
            D.tab = 0;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(t1)) {
            D.tab = 1;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    // --- liste filtree de l'onglet courant ---
    const bool activeTab = D.tab == 0;
    std::vector<downloads::Item> shown;
    for (const auto& x : all)
        if (x.active() == activeTab && matches(x, D.query)) shown.push_back(x);
    sort_items(shown, D.sort);

    // La selection ne doit pas garder d'identifiants disparus : sinon
    // « tout selectionner » puis une action groupee agirait sur du vide.
    for (auto it = D.checked.begin(); it != D.checked.end();) {
        const bool present = std::any_of(
            shown.begin(), shown.end(),
            [&](const downloads::Item& x) { return x.id == *it; });
        it = present ? std::next(it) : D.checked.erase(it);
    }

    // --- actions groupees ---
    const bool allChecked = !shown.empty() && D.checked.size() == shown.size();
    bool selAll = allChecked;
    if (ImGui::Checkbox(tr("Tout sélectionner", "Select all"), &selAll)) {
        D.checked.clear();
        if (selAll)
            for (const auto& x : shown) D.checked.insert(x.id);
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text(tr("%zu sélectionné(s)", "%zu selected"), D.checked.size());
    ImGui::PopStyleColor();
    ImGui::SameLine();

    ImGui::BeginDisabled(D.checked.empty());
    if (activeTab) {
        if (danger_button(tr("Annuler la sélection", "Cancel selected"),
                          ImVec2(210, 0))) {
            for (int id : D.checked) downloads::cancel(id);
            D.checked.clear();
        }
    } else {
        if (ImGui::Button(tr("Relancer la sélection", "Retry selected"),
                          ImVec2(210, 0))) {
            for (int id : D.checked) downloads::retry(id);
            D.checked.clear();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (activeTab) {
        ImGui::BeginDisabled(nActive == 0);
        if (ImGui::Button(tr("Tout annuler", "Cancel all"), ImVec2(160, 0)))
            downloads::cancel_all();
        ImGui::EndDisabled();
    } else {
        ImGui::BeginDisabled(nDone == 0);
        if (ImGui::Button(tr("Effacer les terminés", "Clear finished"),
                          ImVec2(200, 0))) {
            downloads::clear_finished();
            D.checked.clear();
        }
        ImGui::EndDisabled();
    }

    ImGui::Spacing();
    ImGui::BeginChild("##dllist", ImVec2(0, 0));
    if (shown.empty()) {
        if (D.query[0] != '\0') {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(tr("Aucun résultat pour « %s ».",
                                  "No match for \"%s\"."),
                               D.query);
            ImGui::PopStyleColor();
        } else {
            empty_state(activeTab);
        }
    } else {
        for (const auto& x : shown) draw_row(x);
    }
    ImGui::EndChild();
}

} // namespace tl::ui

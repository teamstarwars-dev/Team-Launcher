#include "ui_internal.hpp"

#include "content_install.hpp"
#include "curseforge.hpp"
#include "modrinth.hpp"

#include <functional>
#include <utility>

// ---------------------------------------------------------------------------
// Page Exploration (portage d'ExplorePage.cs) : recherche de mods, modpacks et
// shaders sur Modrinth et CurseForge, installation directe dans une instance.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct Row {
    bool curseforge = false;
    std::string key; // slug Modrinth, ou id numerique CurseForge
    std::string title;
    long long downloads = 0;
    std::string description;
    std::string loaders;
};

struct ExploreState {
    std::mutex m;
    std::thread th;
    bool running = false;
    bool done = false;

    // saisie (main thread)
    char query[96] = "";
    int source = 0; // 0 Modrinth, 1 CurseForge
    int type = 0;   // 0 Modpacks, 1 Mods, 2 Shaders
    bool searched = false;

    // resultats (sous verrou)
    std::vector<Row> rows;
    std::string status;
    std::atomic<bool> cancel{false};

    // installation
    bool installing = false;
    std::string installTitle;
    content::Outcome outcome;
    bool outcomeReady = false;

    // tache AppTasks du job en cours (sous m ; worker la remet a 0)
    int taskId = 0;

    // choix de l'instance de destination
    bool pickOpen = false;
    Row pending;
};
ExploreState E;

content::Category category_of(int type) {
    switch (type) {
    case 1: return content::Category::Mods;
    case 2: return content::Category::Shaders;
    default: return content::Category::Modpacks;
    }
}

void join_finished() {
    std::lock_guard<std::mutex> lk(E.m);
    if (E.done && E.th.joinable()) {
        E.th.join();
        E.done = false;
    }
}

// Demarre un worker ET sa tache AppTasks associee (registre de fond).
// Le job recoit l'id de la tache et doit la cloturer lui-meme via
// apptasks_end() : le panneau reste juste meme si l'utilisateur quitte la
// page. Renvoie false si un job est deja en vol (aucune tache creee).
bool start_job(const std::string& title, const std::string& status,
               const std::function<void(int)>& job) {
    join_finished();
    std::lock_guard<std::mutex> lk(E.m);
    if (E.running) return false;
    if (E.th.joinable()) E.th.join();
    E.running = true;
    E.cancel = false;
    // Creation sous verrou (ordre E.m -> verrous du registre, sans chemin
    // inverse) : taskId reste coherent pour le bouton Annuler de la page.
    const int tid = E.taskId = apptasks_begin(title, status, &E.cancel);
    E.th = std::thread([job, tid] {
        job(tid);
        std::lock_guard<std::mutex> lk(E.m);
        E.running = false;
        E.done = true;
        E.taskId = 0;
    });
    return true;
}

// Flux de recommandations : meme machinerie que la recherche, mais avec
// les facettes de l'instance et un tri par popularite, et sans terme —
// c'est une decouverte, pas une recherche.
void start_recommend(const std::string& loader, const std::string& mcVersion) {
    const content::Category cat = category_of(E.type);
    {
        std::lock_guard<std::mutex> lk(E.m);
        E.rows.clear();
        E.status = tr("Recherche...", "Searching...");
        E.searched = true;
    }
    start_job(tr("Recommandations", "Recommendations"),
              tr("Recherche...", "Searching..."),
              [loader, mcVersion, cat](int tid) {
                  std::vector<Row> rows;
                  std::string status, error;
                  try {
                      for (const auto& h : mr::search_filtered(
                               "", content::category_key(cat), loader,
                               mcVersion, "downloads", &E.cancel)) {
                          Row r;
                          r.key = h.slug;
                          r.title = h.title;
                          r.downloads = h.downloads;
                          r.description = h.description;
                          r.loaders = h.loaders;
                          rows.push_back(std::move(r));
                      }
                      status = rows.empty()
                                   ? std::string(tr(
                                         "Rien de compatible trouvé.",
                                         "Nothing compatible found."))
                                   : std::to_string(rows.size()) +
                                         std::string(tr(" suggestion(s).",
                                                        " suggestion(s)."));
                  } catch (const std::exception& ex) {
                      error = ex.what();
                      status = std::string(tr("Échec : ", "Failed: ")) + error;
                  }
                  apptasks_end(tid, error);
                  std::lock_guard<std::mutex> lk(E.m);
                  E.rows = std::move(rows);
                  E.status = status;
              });
}

void start_search() {
    const std::string q = trimmed(E.query);
    const bool cf_ = E.source == 1;
    const content::Category cat = category_of(E.type);
    {
        std::lock_guard<std::mutex> lk(E.m);
        E.rows.clear();
        E.status = tr("Recherche...", "Searching...");
        E.searched = true;
    }
    const std::string taskTitle = tr("Recherche de contenus", "Content search");
    const std::string taskStatus = tr("Recherche...", "Searching...");
    start_job(taskTitle, taskStatus, [q, cf_, cat](int tid) {
        std::vector<Row> rows;
        std::string status;
        std::string error;
        try {
            if (cf_) {
                for (const auto& h :
                     cf::search(q, content::curseforge_class(cat), &E.cancel)) {
                    Row r;
                    r.curseforge = true;
                    r.key = std::to_string(h.projectId);
                    r.title = h.title;
                    r.downloads = h.downloads;
                    r.description = h.description;
                    r.loaders = h.loaders;
                    rows.push_back(std::move(r));
                }
            } else {
                for (const auto& h :
                     mr::search(q, content::category_key(cat), &E.cancel)) {
                    Row r;
                    r.key = h.slug;
                    r.title = h.title;
                    r.downloads = h.downloads;
                    r.description = h.description;
                    r.loaders = h.loaders;
                    rows.push_back(std::move(r));
                }
            }
            if (rows.empty()) status = tr("Aucun résultat.", "No result.");
        } catch (const std::exception& ex) {
            status = ex.what();
            error = ex.what();
        }
        {
            std::lock_guard<std::mutex> lk(E.m);
            E.rows = std::move(rows);
            E.status = status;
        }
        // Annulation locale non relayee (ex. arret de la page) -> Annulee.
        if (E.cancel.load()) (void)tl::tasks::cancel(tid);
        // Rapport de fin vers le panneau de taches.
        if (error.empty()) {
            if (!status.empty()) tl::tasks::update(tid, status);
            apptasks_end(tid);
        } else {
            apptasks_end(tid, error);
        }
    });
}

void start_install(const Row& row, const nlohmann::json& target) {
    const content::Category cat = category_of(E.type);
    const bool cf_ = row.curseforge;
    const std::string key = row.key;
    const nlohmann::json inst = target;
    {
        std::lock_guard<std::mutex> lk(E.m);
        E.installing = true;
        E.installTitle = row.title;
        E.outcomeReady = false;
        E.status = tr("Téléchargement...", "Downloading...");
    }
    const std::string taskTitle =
        tr("Installation : ", "Installing: ") + row.title;
    const std::string taskStatus = tr("Téléchargement...", "Downloading...");
    start_job(taskTitle, taskStatus, [cf_, key, inst, cat](int tid) {
        content::Outcome o = content::install(cf_, key, inst, cat, E.cancel);
        const bool ok = o.ok;
        std::string error = o.error;
        {
            std::lock_guard<std::mutex> lk(E.m);
            E.outcome = std::move(o);
            E.outcomeReady = true;
            E.installing = false;
            E.status.clear();
        }
        // Annulation locale non relayee (arret de page) -> Annulee, pas
        // « echec » : le registre doit refuser le rapport d'erreur ensuite.
        if (E.cancel.load()) (void)tl::tasks::cancel(tid);
        if (ok) {
            apptasks_end(tid);
        } else {
            // Sinon : echec a afficher (si deja Annulee, sans effet).
            if (error.empty()) error = tr("Installation impossible", "Install failed");
            apptasks_end(tid, error);
        }
    });
}

// Modale « dans quelle instance ? » (portage d'InstancePickDialog).
void draw_instance_picker() {
    if (E.pickOpen) {
        ImGui::OpenPopup("###pickinst");
        E.pickOpen = false;
    }
    const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f,
                   ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 360), ImGuiCond_Appearing);
    const std::string title =
        std::string(tr("Installer dans quelle instance ?")) + "###pickinst";
    bool open = true;
    if (!ImGui::BeginPopupModal(title.c_str(), &open,
                                ImGuiWindowFlags_NoCollapse |
                                    ImGuiWindowFlags_NoResize))
        return;

    auto& arr = inst_array();
    if (!arr.is_array() || arr.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucune instance trouvée. Crée-en une d'abord !"));
        ImGui::PopStyleColor();
    } else {
        ImGui::BeginChild("##picklist", ImVec2(0, 250));
        for (auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string label = e.value("Name", "?") + "  ·  " +
                                      e.value("Loader", "Vanilla") + " " +
                                      e.value("McVersion", "?");
            if (ImGui::Selectable(label.c_str())) {
                start_install(E.pending, e);
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndChild();
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Annuler"), ImVec2(120, 34))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void draw_card(const Row& r, bool busy) {
    ImGui::PushID(r.key.c_str());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##card", ImVec2(0, 96), ImGuiChildFlags_Borders);

    ImGui::TextUnformatted(r.title.c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (fSmall) ImGui::PushFont(fSmall);
    // Telechargements en milliers/millions, comme les boutiques.
    char dl[48];
    if (r.downloads >= 1000000)
        std::snprintf(dl, sizeof(dl), "%.1f M", r.downloads / 1000000.0);
    else if (r.downloads >= 1000)
        std::snprintf(dl, sizeof(dl), "%.0f k", r.downloads / 1000.0);
    else
        std::snprintf(dl, sizeof(dl), "%lld", r.downloads);
    ImGui::Text("%s %s%s%s", dl, tr("téléchargements", "downloads"),
                r.loaders.empty() ? "" : "  ·  ", r.loaders.c_str());
    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - 140.0f);
    ImGui::TextUnformatted(r.description.c_str());
    ImGui::PopTextWrapPos();
    if (fSmall) ImGui::PopFont();
    ImGui::PopStyleColor();

    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 132.0f, 30.0f));
    ImGui::BeginDisabled(busy);
    if (accent_button(tr("Installer"), ImVec2(120, 34))) {
        if (category_of(E.type) == content::Category::Modpacks) {
            // Un modpack cree sa propre instance : pas de cible a choisir.
            start_install(r, nlohmann::json(nullptr));
        } else {
            E.pending = r;
            E.pickOpen = true;
        }
    }
    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
    ImGui::Spacing();
}

} // namespace

void explore_page() {
    join_finished();

    // TL_AUTO_SEARCH=<terme> : lance une recherche sans clic (test).
    // TL_AUTO_SOURCE=0|1 (Modrinth/CurseForge), TL_AUTO_TYPE=0|1|2.
    static bool autoDone = false;
    if (!autoDone) {
        autoDone = true;
        if (const char* q = std::getenv("TL_AUTO_SEARCH")) {
            std::snprintf(E.query, sizeof(E.query), "%s", q);
            if (const char* s = std::getenv("TL_AUTO_SOURCE")) E.source = std::atoi(s);
            if (const char* t = std::getenv("TL_AUTO_TYPE")) E.type = std::atoi(t);
            start_search();
        }
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Exploration"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", tr("Mods, modpacks et shaders de Modrinth et "
                                "CurseForge - Forge, Fabric, NeoForge, Quilt...",
                                "Mods, modpacks and shaders from Modrinth and "
                                "CurseForge - Forge, Fabric, NeoForge, Quilt..."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    bool busy;
    std::string status;
    bool outcomeReady;
    {
        std::lock_guard<std::mutex> lk(E.m);
        busy = E.running;
        status = E.status;
        outcomeReady = E.outcomeReady;
    }

    // ---- Barre de recherche ----
    ImGui::SetNextItemWidth(320.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##q", tr("Recherche un mod, un modpack..."), E.query, sizeof(E.query),
        ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const char* sources[] = {"Modrinth", "CurseForge"};
    ImGui::SetNextItemWidth(140.0f);
    ImGui::Combo("##src", &E.source, sources, 2);
    ImGui::SameLine();
    const char* types[] = {tr("Modpacks"), tr("Mods"), tr("Shaders")};
    ImGui::SetNextItemWidth(140.0f);
    ImGui::Combo("##type", &E.type, types, 3);
    ImGui::SameLine();
    ImGui::BeginDisabled(busy);
    const bool go = accent_button(tr("Rechercher"), ImVec2(130, 34));
    ImGui::EndDisabled();
    if ((go || enter) && !busy) start_search();

    // ---- Flux de recommandations (phase 5) ----
    // Un palmares generique n'aide personne : le mod le plus telecharge du
    // moment ne sert a rien s'il ne tourne pas sur la version qu'on joue.
    // On filtre donc sur le chargeur et la version de l'instance
    // selectionnee, et on trie par popularite. Sans instance, le bouton
    // n'a rien sur quoi s'appuyer : on le grise plutot que de proposer un
    // classement au hasard.
    {
        const nlohmann::json* inst = selected_instance();
        const std::string ldr = inst ? inst->value("Loader", "") : std::string{};
        std::string mc = inst ? inst->value("McVersion", "") : std::string{};
        if (mc == "latest" || mc == "release" || mc == "snapshot") mc.clear();
        const bool usable = inst && E.source == 0; // facettes : Modrinth seul
        ImGui::BeginDisabled(busy || !usable);
        if (ImGui::Button(tr("Recommandés pour mon instance",
                             "Recommended for my instance"),
                          ImVec2(260, 30)))
            start_recommend(ldr, mc);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (!inst)
            ImGui::TextUnformatted(tr("Choisissez d'abord une instance.",
                                      "Pick an instance first."));
        else if (E.source != 0)
            ImGui::TextUnformatted(
                tr("Disponible sur Modrinth uniquement.",
                   "Available on Modrinth only."));
        else
            ImGui::Text(tr("Les plus installés compatibles %s%s%s.",
                           "Most installed, compatible with %s%s%s."),
                        ldr.empty() ? "?" : ldr.c_str(),
                        mc.empty() ? "" : " - ", mc.c_str());
        ImGui::PopStyleColor();
    }

    // CurseForge sans cle : on le dit avant que l'utilisateur cherche.
    if (E.source == 1 && !cf::has_key()) {
        ImGui::Spacing();
        // Orange et non rouge : ce n'est pas une panne, c'est une source
        // indisponible parmi deux. Et on dit ce qui marche avant ce qui
        // manque — l'utilisateur veut chercher un mod, pas lire un
        // diagnostic.
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE0A030));
        ImGui::TextWrapped(
            "%s",
            tr("CurseForge indisponible dans cette version : bascule sur "
               "Modrinth, qui ne demande aucune clé. Pour activer "
               "CurseForge, colle une clé API dans Paramètres > "
               "Intégrations.",
               "CurseForge unavailable in this build: switch to Modrinth, "
               "which needs no key. To enable CurseForge, paste an API key "
               "in Settings > Integrations."));
        ImGui::PopStyleColor();
    }

    // ---- Resultat d'une installation ----
    if (outcomeReady) {
        content::Outcome o;
        std::string what;
        {
            std::lock_guard<std::mutex> lk(E.m);
            o = std::move(E.outcome);
            what = E.installTitle;
            E.outcomeReady = false;
        }
        if (!o.ok) {
            notify_toast(tr("Installation impossible"), o.error);
        } else if (o.instance.is_object()) {
            // Modpack : nouvelle instance a enregistrer (main thread).
            inst_array().push_back(o.instance);
            DataStore::save();
            g.countsDirty = true;
            notify_toast(tr("Modpack installé"),
                         "« " + o.instance.value("Name", what) + " »" +
                             tr(" ajouté à tes instances.",
                                " added to your instances."));
        } else {
            notify_toast(tr("Installé"),
                         "« " + what + " »" +
                             tr(" a été ajouté à l'instance.",
                                " was added to the instance."));
        }
    }

    // ---- Etat / progression ----
    if (busy || !status.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, busy ? kDim : kDanger);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
        if (busy) {
            ImGui::SameLine();
            if (ImGui::Button(tr("Annuler"), ImVec2(90, 0))) E.cancel = true;
        }
    }

    // ---- Resultats ----
    ImGui::Spacing();
    std::vector<Row> rows;
    {
        std::lock_guard<std::mutex> lk(E.m);
        rows = E.rows;
    }
    if (rows.empty() && !busy && !E.searched) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Lance une recherche pour voir des contenus.",
                                    "Run a search to browse content."));
        ImGui::PopStyleColor();
    }
    ImGui::BeginChild("##results");
    for (const auto& r : rows) draw_card(r, busy);
    ImGui::EndChild();

    draw_instance_picker();
}

void explore_stop() {
    E.cancel = true;
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(E.m);
        th = std::move(E.th);
    }
    if (th.joinable()) th.join();
}


// Recherche pilotee depuis une autre page (suggestions de compatibilite,
// page 5 : « complements possibles »). On force le type « Mods » : une
// suggestion issue des dependances est toujours un mod, jamais un modpack.
void explore_search(const std::string& term) {
    std::snprintf(E.query, sizeof(E.query), "%s", term.c_str());
    E.type = 1;
    start_search();
}

} // namespace tl::ui

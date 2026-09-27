#include "ui_internal.hpp"

#include "region.hpp"
#include "world.hpp"
#include "worldsync.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>

// ---------------------------------------------------------------------------
// Page Explorateur (portage d'ExplorerPage.cs) : navigation dans les fichiers
// des instances, avec deux vues — « Mondes » (lecture reelle de level.dat) et
// « Fichiers » (arborescence brute, comme le C#).
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct Entry {
    std::filesystem::path path;
    std::string name;
    bool dir = false;
    std::int64_t size = 0;
};

struct ExplorerState {
    int tab = 0;                       // 0 Mondes, 1 Fichiers
    int pendingTab = -1;               // bascule programmee (une seule frame)
    std::string instId;                // instance selectionnee
    std::filesystem::path cwd;         // dossier courant (vue Fichiers)
    std::vector<std::filesystem::path> history;
    std::vector<Entry> entries;
    bool dirty = true;

    std::vector<world::Info> worlds;
    std::string worldsFor;             // instance dont `worlds` provient
    bool worldsDirty = true;
    int selWorld = -1;
};
ExplorerState E;

std::string human_size(std::int64_t n) {
    char b[32];
    if (n >= 1024LL * 1024 * 1024)
        std::snprintf(b, sizeof(b), "%.1f Go", n / 1024.0 / 1024.0 / 1024.0);
    else if (n >= 1024 * 1024)
        std::snprintf(b, sizeof(b), "%.1f Mo", n / 1024.0 / 1024.0);
    else if (n >= 1024)
        std::snprintf(b, sizeof(b), "%.0f Ko", n / 1024.0);
    else
        std::snprintf(b, sizeof(b), "%lld o", static_cast<long long>(n));
    return b;
}

// epoch ms (Java) -> "JJ/MM/AAAA HH:MM"
std::string fmt_ms(std::int64_t ms) {
    if (ms <= 0) return tr("jamais", "never");
    const std::time_t t = static_cast<std::time_t>(ms / 1000);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &tmv);
    return buf;
}

const char* game_type_name(int gt) {
    switch (gt) {
    case 0: return tr("Survie", "Survival");
    case 1: return tr("Créatif", "Creative");
    case 2: return tr("Aventure", "Adventure");
    case 3: return tr("Spectateur", "Spectator");
    default: return "?";
    }
}

std::filesystem::path instance_root() {
    if (E.instId.empty()) return DataStore::instancesRoot();
    return DataStore::instancesRoot() / E.instId;
}

void refresh_entries() {
    E.entries.clear();
    E.dirty = false;
    std::error_code ec;
    if (E.cwd.empty()) E.cwd = instance_root();
    if (!std::filesystem::is_directory(E.cwd, ec)) return;
    for (const auto& e : std::filesystem::directory_iterator(E.cwd, ec)) {
        if (ec) break;
        Entry it;
        it.path = e.path();
        it.name = e.path().filename().string();
        std::error_code e2;
        it.dir = e.is_directory(e2);
        if (!it.dir) it.size = static_cast<std::int64_t>(e.file_size(e2));
        E.entries.push_back(std::move(it));
    }
    // Dossiers d'abord, puis par nom (insensible a la casse).
    std::sort(E.entries.begin(), E.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

void navigate(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_directory(p, ec)) return;
    if (!E.cwd.empty()) E.history.push_back(E.cwd);
    E.cwd = p;
    E.dirty = true;
}

void go_back() {
    if (E.history.empty()) return;
    E.cwd = E.history.back();
    E.history.pop_back();
    E.dirty = true;
}

void go_up() {
    // On ne remonte jamais au-dessus de la racine des instances.
    const auto root = DataStore::instancesRoot();
    std::error_code ec;
    if (std::filesystem::equivalent(E.cwd, root, ec)) return;
    const auto parent = E.cwd.parent_path();
    if (parent.empty() || parent == E.cwd) return;
    navigate(parent);
}

// Selecteur d'instance partage par les deux vues.
void instance_picker() {
    auto& arr = inst_array();
    std::string label = tr("Toutes les instances", "All instances");
    if (!E.instId.empty())
        if (const auto* e = find_instance(E.instId)) label = e->value("Name", "?");

    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::BeginCombo("##explinst", label.c_str())) {
        if (ImGui::Selectable(tr("Toutes les instances", "All instances"),
                              E.instId.empty())) {
            E.instId.clear();
            E.cwd.clear();
            E.history.clear();
            E.dirty = true;
            E.worldsDirty = true;
        }
        if (arr.is_array())
            for (auto& e : arr) {
                if (!e.is_object()) continue;
                const std::string id = e.value("Id", "");
                if (id.empty()) continue;
                if (ImGui::Selectable(e.value("Name", "?").c_str(), id == E.instId)) {
                    E.instId = id;
                    E.cwd.clear();
                    E.history.clear();
                    E.dirty = true;
                    E.worldsDirty = true;
                }
            }
        ImGui::EndCombo();
    }
}

// --- vue Mondes ------------------------------------------------------------

void refresh_worlds() {
    E.worlds.clear();
    E.worldsDirty = false;
    E.worldsFor = E.instId;
    E.selWorld = -1;
    auto& arr = inst_array();
    if (!E.instId.empty()) {
        E.worlds = world::list_worlds(DataStore::instancesRoot() / E.instId);
        return;
    }
    // Toutes les instances : on agrege.
    if (arr.is_array())
        for (auto& e : arr) {
            if (!e.is_object()) continue;
            const std::string id = e.value("Id", "");
            if (id.empty()) continue;
            for (auto& w : world::list_worlds(DataStore::instancesRoot() / id))
                E.worlds.push_back(std::move(w));
        }
    std::sort(E.worlds.begin(), E.worlds.end(),
              [](const world::Info& a, const world::Info& b) {
                  return a.lastPlayed > b.lastPlayed;
              });
}

void tab_worlds() {
    if (E.worldsDirty || E.worldsFor != E.instId) refresh_worlds();

    ImGui::SameLine();
    if (ImGui::Button(tr("Actualiser"), ImVec2(120, 0))) E.worldsDirty = true;
    ImGui::Spacing();

    if (E.worlds.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucun monde trouvé."));
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginChild("##worlds");
    for (size_t i = 0; i < E.worlds.size(); ++i) {
        const world::Info& w = E.worlds[i];
        ImGui::PushID(static_cast<int>(i));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::BeginChild("##w", ImVec2(0, 92), ImGuiChildFlags_Borders);

        ImGui::TextUnformatted(w.name.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (fSmall) ImGui::PushFont(fSmall);
        const std::string ver = world::version_name(w.dataVersion);
        ImGui::Text("%s  ·  %s%s  ·  %s  ·  %d %s", game_type_name(w.gameType),
                    ver.empty() ? "?" : ver.c_str(),
                    w.hardcore ? (tr(" · extrême", " · hardcore")) : "",
                    human_size(w.sizeBytes).c_str(), w.regionCount,
                    tr("région(s)", "region(s)"));
        ImGui::Text("%s%s  ·  %s", tr("Dernière partie : ", "Last played: "),
                    fmt_ms(w.lastPlayed).c_str(), w.folder.c_str());
        if (fSmall) ImGui::PopFont();
        ImGui::PopStyleColor();

        ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - 312.0f, 28.0f));
        if (ImGui::Button(tr("Ouvrir le dossier"), ImVec2(160, 30)))
            open_in_explorer(w.path);
        ImGui::SameLine();
        if (ImGui::Button(tr("Fichiers"), ImVec2(120, 30))) {
            E.cwd = w.path;
            E.history.clear();
            E.dirty = true;
            E.pendingTab = 1; // bascule ponctuelle, pas a chaque frame
        }

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

// --- vue Fichiers ----------------------------------------------------------

void tab_files() {
    if (E.dirty) refresh_entries();

    ImGui::SameLine();
    ImGui::BeginDisabled(E.history.empty());
    if (ImGui::Button(tr("Retour", "Back"), ImVec2(90, 0))) go_back();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Dossier parent", "Parent folder"), ImVec2(150, 0))) go_up();
    ImGui::SameLine();
    if (ImGui::Button(tr("Ouvrir dans Windows"), ImVec2(200, 0)))
        open_in_explorer(E.cwd);

    // Barre d'adresse en lecture seule (le C# la laissait editable mais ne
    // validait rien ; ici on ne propose pas d'y taper un chemin arbitraire).
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", E.cwd.string().c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (E.entries.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Dossier vide."));
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginChild("##files");
    for (size_t i = 0; i < E.entries.size(); ++i) {
        const Entry& e = E.entries[i];
        ImGui::PushID(static_cast<int>(i));
        const std::string label =
            (e.dir ? std::string("[") + tr("dossier", "folder") + "]  " : "   ") +
            e.name;
        if (ImGui::Selectable(label.c_str(), false,
                              ImGuiSelectableFlags_AllowDoubleClick)) {
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                if (e.dir)
                    navigate(e.path);
                else
                    open_in_explorer(e.path); // ouvre avec l'appli associee
            }
        }
        if (!e.dir) {
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (fSmall) ImGui::PushFont(fSmall);
            ImGui::TextUnformatted(human_size(e.size).c_str());
            if (fSmall) ImGui::PopFont();
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// --- vue Synchronisation CurseForge ----------------------------------------

struct SyncState {
    std::vector<wsync::Compare> cmp;
    bool loaded = false;
    std::string status;
};
SyncState S;

void tab_sync() {
    ImGui::SameLine();
    if (ImGui::Button(tr("Analyser", "Scan"), ImVec2(120, 0))) {
        S.cmp = wsync::compare_all();
        S.loaded = true;
        S.status.clear();
    }
    ImGui::Spacing();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s", tr("Compare tes instances CurseForge avec celles du launcher qui "
                 "portent le même nom, et importe les mondes plus récents. Le "
                 "monde remplacé est archivé avant tout écrasement.",
                 "Compares your CurseForge instances with the launcher ones "
                 "sharing the same name, and imports newer worlds. Any replaced "
                 "world is archived before being overwritten."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (!S.loaded) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Clique sur « Analyser » pour comparer.",
                                  "Click \"Scan\" to compare."));
        ImGui::PopStyleColor();
        return;
    }
    if (!S.status.empty()) {
        ImGui::TextUnformatted(S.status.c_str());
        ImGui::Spacing();
    }
    if (S.cmp.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucune instance CurseForge à rapprocher.",
                                    "No CurseForge instance to reconcile."));
        ImGui::PopStyleColor();
        return;
    }

    ImGui::BeginChild("##sync");
    for (size_t ci = 0; ci < S.cmp.size(); ++ci) {
        const auto& c = S.cmp[ci];
        ImGui::PushID(static_cast<int>(ci));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        const float h = 56.0f + (c.newer.size() + c.onlyInCurseForge.size()) * 30.0f;
        ImGui::BeginChild("##c", ImVec2(0, (std::min)(h, 260.0f)),
                          ImGuiChildFlags_Borders);

        ImGui::TextUnformatted(c.instanceName.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (fSmall) ImGui::PushFont(fSmall);
        if (c.launcherInstanceId.empty())
            ImGui::TextUnformatted(
                tr("Aucune instance du launcher de ce nom : import impossible.",
                   "No launcher instance with this name: cannot import."));
        else
            ImGui::Text("%zu %s  ·  %zu %s  ·  %zu %s", c.newer.size(),
                        tr("plus récent(s)", "newer"), c.onlyInCurseForge.size(),
                        tr("absent(s) ici", "missing here"),
                        c.onlyInLauncher.size(),
                        tr("absent(s) côté CurseForge", "missing on CurseForge"));
        if (fSmall) ImGui::PopFont();
        ImGui::PopStyleColor();

        auto row = [&](const wsync::Snapshot& w, const char* badge) {
            ImGui::PushID(w.worldFolder.c_str());
            ImGui::Text("%s  ·  %s", w.displayName.c_str(), badge);
            ImGui::SameLine(ImGui::GetContentRegionAvail().x - 110.0f);
            ImGui::BeginDisabled(c.launcherInstanceId.empty());
            if (accent_button(tr("Importer"), ImVec2(110, 24))) {
                auto r = wsync::import_world(w, c.launcherInstanceId);
                if (r.ok) {
                    S.status = "« " + w.displayName + " »" +
                               tr(" importé.", " imported.");
                    if (!r.backup.empty())
                        S.status += tr(" Ancien monde archivé.",
                                       " Previous world archived.");
                    notify_toast(tr("Monde importé", "World imported"), S.status);
                    S.cmp = wsync::compare_all();
                } else {
                    notify_toast(tr("Import impossible", "Import failed"), r.error);
                }
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        };
        for (const auto& w : c.newer) row(w, tr("plus récent", "newer"));
        for (const auto& w : c.onlyInCurseForge)
            row(w, tr("absent du launcher", "not in launcher"));

        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopID();
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

} // namespace

void explorer_page() {
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Explorateur"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", tr("Fichiers et dossiers de tes instances."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##expltabs")) {
        ImGui::PushStyleColor(ImGuiCol_Tab, kCard);
        ImGui::PushStyleColor(ImGuiCol_TabActive, kAccent);
        auto forced = [](int i) {
            if (E.pendingTab != i) return ImGuiTabItemFlags_None;
            E.pendingTab = -1;
            return ImGuiTabItemFlags_SetSelected;
        };
        if (ImGui::BeginTabItem(tr("Mondes"), nullptr, forced(0))) {
            E.tab = 0;
            instance_picker();
            tab_worlds();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Fichiers"), nullptr, forced(1))) {
            E.tab = 1;
            instance_picker();
            tab_files();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Synchronisation", "Sync"), nullptr, forced(2))) {
            E.tab = 2;
            ImGui::TextUnformatted(tr("Instances CurseForge du PC",
                                      "CurseForge instances on this PC"));
            tab_sync();
            ImGui::EndTabItem();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndTabBar();
    }
}

} // namespace tl::ui

#include "cf_import.hpp"

#include "apptasks.hpp"
#include "datastore.hpp"    // make_instance, DataStore
#include "pack_import.hpp"  // detect_folder

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <mutex>
#include <set>
#include <string>

namespace fs = std::filesystem;

namespace tl::cfimport {

namespace {

std::mutex g_mutex;
std::vector<nlohmann::json> g_ready;
bool g_started = false;

std::string env_str(const char* k) {
    const char* v = std::getenv(k);
    return v ? v : std::string{};
}

// Dossier d'instances de l'app CurseForge : réglage si renseigné, sinon défaut.
fs::path instances_dir() {
    const std::string& cfg = DataStore::settings.curseForgeInstancesDir;
    if (!cfg.empty()) return fs::path(cfg);
#ifdef _WIN32
    const std::string home = env_str("USERPROFILE");
#else
    const std::string home = env_str("HOME");
#endif
    if (home.empty()) return {};
    return fs::path(home) / "curseforge" / "minecraft" / "Instances";
}

} // namespace

void start_async() {
    if (!DataStore::settings.autoImportCurseForge) return;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_started) return;
        g_started = true;
    }

    // Snapshot des instances déjà importées, lu ICI (fil UI, démarrage) pour ne
    // pas courir avec d'éventuelles écritures ultérieures de la liste.
    std::set<std::string> already;
    for (const auto& inst : DataStore::settings.instances)
        if (inst.is_object()) {
            const std::string s = inst.value("CfSource", std::string{});
            if (!s.empty()) already.insert(s);
        }
    const fs::path root = instances_dir();

    tasks::run(
        "Import CurseForge",
        [already, root](const std::atomic<bool>& cancel,
                        const std::function<void(const std::string&, double)>& set_status) {
            std::error_code ec;
            if (root.empty() || !fs::is_directory(root, ec)) {
                set_status("Aucun dossier CurseForge détecté.", 1.0);
                return;
            }
            std::vector<fs::path> dirs;
            for (const auto& e : fs::directory_iterator(root, ec)) {
                if (e.is_directory(ec) &&
                    fs::exists(e.path() / "minecraftinstance.json", ec))
                    dirs.push_back(e.path());
            }

            int done = 0, imported = 0;
            const double total = static_cast<double>(dirs.size());
            for (const auto& dir : dirs) {
                if (cancel.load()) break;
                ++done;
                const std::string src = dir.string();
                if (already.count(src)) continue; // déjà importée

                const auto info = packs::detect_folder(dir);
                const std::string name =
                    info.name.empty() ? dir.filename().string() : info.name;
                set_status("Import « " + name + " »...",
                           total > 0 ? done / total : -1.0);

                nlohmann::json inst = make_instance(name, info.loader, info.mcVersion);
                inst["CfSource"] = src; // dédup au prochain démarrage
                inst["Description"] =
                    "Importée de CurseForge (" + info.loader + " " + info.mcVersion + ")";
                const fs::path destDir =
                    DataStore::instancesRoot() / inst.value("Id", std::string{});
                fs::copy(info.gameDir, destDir,
                         fs::copy_options::recursive |
                             fs::copy_options::overwrite_existing,
                         ec);
                if (ec) {
                    ec.clear();
                    continue; // copie échouée : on saute cette instance
                }
                {
                    std::lock_guard<std::mutex> lk(g_mutex);
                    g_ready.push_back(std::move(inst));
                }
                ++imported;
            }
            set_status(imported ? ("Importé : " + std::to_string(imported) +
                                   " instance(s) CurseForge.")
                                : "Aucune nouvelle instance CurseForge.",
                       1.0);
        });
}

std::vector<nlohmann::json> take_ready() {
    std::lock_guard<std::mutex> lk(g_mutex);
    std::vector<nlohmann::json> out;
    out.swap(g_ready);
    return out;
}

} // namespace tl::cfimport

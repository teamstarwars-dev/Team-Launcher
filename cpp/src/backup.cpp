#include "backup.hpp"

#include "datastore.hpp"
#include "util_zip.hpp"

#include <atomic>
#include <thread>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <random>

namespace fs = std::filesystem;

namespace tl::backup {

namespace {

fs::path saves_dir(const std::string& instanceId) {
    return DataStore::instancesRoot() / instanceId / "saves";
}

long long mtime_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    // file_time_type -> epoch s. Unix (tri/rotation).
    return file_time_to_unix(t);
}

bool dir_has_entries(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return false;
    fs::directory_iterator it(p, ec);
    return !ec && it != fs::directory_iterator();
}

} // namespace

fs::path dir(const std::string& instanceId) {
    return DataStore::instancesRoot() / instanceId / "backups";
}

std::string create(const std::string& instanceId) {
    const fs::path saves = saves_dir(instanceId);
    if (!dir_has_entries(saves)) return {};

    const fs::path out = dir(instanceId);
    std::error_code ec;
    fs::create_directories(out, ec);

    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char stamp[32];
    std::strftime(stamp, sizeof(stamp), "mondes-%Y-%m-%d_%H-%M.zip", &tmv);

    const fs::path path = out / stamp;
    fs::remove(path, ec); // C# : File.Delete si deja present (meme minute)
    if (!zip_create_from_dir(saves, path)) return {};

    // Rotation : ne garder que les kMaxBackups plus recentes.
    auto all = list(instanceId);
    for (size_t i = kMaxBackups; i < all.size(); ++i) fs::remove(all[i].file, ec);

    return path.string();
}

std::vector<Entry> list(const std::string& instanceId) {
    std::vector<Entry> out;
    const fs::path d = dir(instanceId);
    std::error_code ec;
    if (!fs::is_directory(d, ec)) return out;
    for (const auto& e : fs::directory_iterator(d, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".zip") continue;
        std::error_code se;
        const long long sz = static_cast<long long>(fs::file_size(e.path(), se));
        out.push_back({e.path(), mtime_of(e.path()), se ? 0 : sz});
    }
    std::sort(out.begin(), out.end(),
              [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
    return out;
}

bool restore(const std::string& instanceId, const fs::path& zip) {
    const fs::path saves = saves_dir(instanceId);
    std::error_code ec;

    // Dossier temporaire a cote de l'instance (meme volume -> rename atomique).
    std::random_device rd;
    char suffix[32]; // "restore-" + 16 hex + NUL = 25
    std::snprintf(suffix, sizeof(suffix), "restore-%08x%08x", rd(), rd());
    const fs::path temp = DataStore::instancesRoot() / instanceId / suffix;

    bool ok = false;
    if (zip_extract_all(zip, temp) >= 0) {
        // Divergence assumee vs C# : on n'efface l'ancien `saves` qu'une fois
        // l'extraction reussie (le C# supprimait avant, perte seche si echec).
        fs::remove_all(saves, ec);
        fs::create_directories(saves.parent_path(), ec);
        fs::rename(temp, saves, ec);
        ok = !ec;
    }
    fs::remove_all(temp, ec);
    return ok;
}

bool remove(const fs::path& zip) {
    std::error_code ec;
    return fs::remove(zip, ec) && !ec;
}

// --- Rotation ---------------------------------------------------------------

int rotate(const std::string& instanceId, int keep, long long maxBytes) {
    auto entries = list(instanceId); // deja de la plus recente a la plus ancienne
    int removed = 0;

    // 1. Trop nombreuses : on coupe la queue.
    if (keep > 0 && entries.size() > static_cast<std::size_t>(keep)) {
        for (std::size_t i = static_cast<std::size_t>(keep); i < entries.size();
             ++i)
            if (tl::backup::remove(entries[i].file)) ++removed;
        entries.resize(static_cast<std::size_t>(keep));
    }

    // 2. Trop volumineuses : on retire les plus anciennes jusqu'a repasser
    //    sous le quota. On garde TOUJOURS la plus recente, meme si elle
    //    depasse a elle seule : supprimer la seule sauvegarde existante
    //    pour respecter un quota serait le contraire du but recherche.
    if (maxBytes > 0) {
        long long total = 0;
        for (const auto& e : entries) total += e.bytes;
        for (std::size_t i = entries.size(); i-- > 1 && total > maxBytes;) {
            if (tl::backup::remove(entries[i].file)) {
                total -= entries[i].bytes;
                ++removed;
            }
        }
    }
    return removed;
}

// --- Sauvegarde automatique -------------------------------------------------

bool is_due(long long nowUnix, long long lastUnix, int everyHours) {
    if (everyHours <= 0) return false;      // desactive
    if (lastUnix <= 0) return true;         // jamais sauvegarde
    if (nowUnix < lastUnix) return false;   // horloge reculee : on s'abstient
    return nowUnix - lastUnix >= static_cast<long long>(everyHours) * 3600;
}

long long last_backup_unix(const std::string& instanceId) {
    const auto v = list(instanceId);
    return v.empty() ? 0 : v.front().mtime;
}

namespace {

std::thread g_timer;
std::atomic<bool> g_stop{false};
std::atomic<bool> g_gameRunning{false};

void timer_loop() {
    // Reveil toutes les minutes : assez fin pour un intervalle exprime en
    // heures, assez rare pour ne rien couter.
    while (!g_stop.load()) {
        for (int i = 0; i < 60 && !g_stop.load(); ++i)
            std::this_thread::sleep_for(std::chrono::seconds(1));
        if (g_stop.load()) break;

        const auto& s = DataStore::settings;
        const int every = s.backupAutoHours;
        if (every <= 0) continue;
        // Jamais pendant une partie : zipper un monde en cours d'ecriture
        // donnerait une archive incoherente, donc inutilisable.
        if (g_gameRunning.load()) continue;

        const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                             std::chrono::system_clock::now().time_since_epoch())
                             .count();
        for (const auto& inst : s.instances) {
            if (g_stop.load()) break;
            if (!inst.is_object()) continue;
            const std::string id = inst.value("Id", "");
            if (id.empty()) continue;
            if (!is_due(now, last_backup_unix(id), every)) continue;
            if (create(id).empty()) continue; // pas de saves : rien a faire
            rotate(id, s.backupKeep > 0 ? s.backupKeep : kMaxBackups,
                   static_cast<long long>(s.backupSpaceMb) * 1024 * 1024);
        }
    }
}

} // namespace

void set_game_running(bool running) { g_gameRunning.store(running); }

void auto_start() {
    if (g_timer.joinable()) return;
    g_stop = false;
    g_timer = std::thread(timer_loop);
}

void auto_stop() {
    g_stop = true;
    if (g_timer.joinable()) g_timer.join();
}

} // namespace tl::backup

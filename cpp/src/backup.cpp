#include "backup.hpp"

#include "datastore.hpp"
#include "util_zip.hpp"

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
    // file_time_type -> epoch s. (suffisant pour trier/afficher)
    return std::chrono::duration_cast<std::chrono::seconds>(
               t.time_since_epoch())
        .count();
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
    localtime_s(&tmv, &t);
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
        out.push_back({e.path(), mtime_of(e.path())});
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

} // namespace tl::backup

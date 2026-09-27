#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shlobj.h>

#include "worldsync.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "nbt.hpp"
#include "util_zip.hpp"
#include "world.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::wsync {

namespace {

// Dossiers que le C# excluait d'un import (PackService.Excluded).
bool excluded_name(const std::string& n) {
    static const char* const kEx[] = {"logs",    "crash-reports", "screenshots",
                                      "backups", ".mixin.out",    "cache"};
    for (const char* e : kEx)
        if (_stricmp(n.c_str(), e) == 0) return true;
    return false;
}

std::int64_t mtime_of(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch())
        .count();
}

// Date du dossier de monde : le C# lisait LastWriteTime du dossier lui-meme,
// que Windows ne met pas a jour quand un fichier imbrique change. On prend la
// date la plus recente rencontree dans l'arborescence : c'est ce que « monde
// modifie en jeu » veut dire.
std::int64_t newest_mtime(const fs::path& dir) {
    std::int64_t best = mtime_of(dir);
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (!it->is_regular_file(e2)) continue;
        best = (std::max)(best, mtime_of(it->path()));
    }
    return best;
}

fs::path user_profile() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) return {};
    fs::path out(p);
    CoTaskMemFree(p);
    return out;
}

std::string iso_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &tmv);
    return buf;
}

bool copy_tree(const fs::path& src, const fs::path& dst, std::string* err) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    for (fs::recursive_directory_iterator it(src, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            if (err) *err = "Lecture impossible : " + ec.message();
            return false;
        }
        std::error_code e2;
        const fs::path rel = fs::relative(it->path(), src, e2);
        if (e2 || rel.empty()) continue;
        const fs::path target = dst / rel;
        if (it->is_directory(e2)) {
            fs::create_directories(target, e2);
            continue;
        }
        if (!it->is_regular_file(e2)) continue;
        fs::create_directories(target.parent_path(), e2);
        fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            if (err) *err = "Copie impossible : " + rel.string();
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<std::pair<fs::path, std::string>> detect_curseforge_instances() {
    std::vector<std::pair<fs::path, std::string>> out;
    const fs::path base = user_profile() / "curseforge" / "minecraft" / "Instances";
    std::error_code ec;
    if (!fs::is_directory(base, ec)) return out;
    for (const auto& e : fs::directory_iterator(base, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        const std::string n = e.path().filename().string();
        if (excluded_name(n)) continue;
        out.emplace_back(e.path(), n);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return _stricmp(a.second.c_str(), b.second.c_str()) < 0;
    });
    return out;
}

std::vector<Snapshot> list_worlds_in(const fs::path& instanceRoot, Origin origin) {
    std::vector<Snapshot> out;
    std::error_code ec;
    if (!fs::is_directory(instanceRoot, ec)) return out;

    // CurseForge : <instance>/minecraft/saves ; launcher : <instance>/saves.
    const fs::path candidates[] = {instanceRoot / "minecraft" / "saves",
                                   instanceRoot / "saves"};
    for (const fs::path& saves : candidates) {
        if (!fs::is_directory(saves, ec)) continue;
        for (const auto& e : fs::directory_iterator(saves, ec)) {
            if (ec) break;
            if (!e.is_directory(ec)) continue;
            // Un monde a forcement un level.dat.
            if (!fs::is_regular_file(e.path() / "level.dat", ec)) continue;

            Snapshot s;
            s.instanceName = instanceRoot.filename().string();
            s.worldFolder = e.path().filename().string();
            s.path = e.path();
            s.origin = origin;
            s.displayName = s.worldFolder;
            if (auto info = world::read_level(e.path())) {
                if (!info->name.empty()) s.displayName = info->name;
                s.levelLastPlayed = info->lastPlayed;
                s.sizeBytes = info->sizeBytes;
            } else {
                s.sizeBytes = world::dir_size(e.path());
            }
            s.lastModified = newest_mtime(e.path());
            out.push_back(std::move(s));
        }
    }
    return out;
}

std::vector<Compare> compare_all() {
    std::vector<Compare> out;
    auto& arr = DataStore::settings.instances;

    for (const auto& [cfPath, cfName] : detect_curseforge_instances()) {
        Compare c;
        c.instanceName = cfName;
        c.curseForgePath = cfPath;

        // Instance du launcher de meme nom (insensible a la casse).
        const json* match = nullptr;
        if (arr.is_array())
            for (const auto& e : arr) {
                if (!e.is_object()) continue;
                if (_stricmp(e.value("Name", "").c_str(), cfName.c_str()) == 0) {
                    match = &e;
                    break;
                }
            }
        if (match) c.launcherInstanceId = match->value("Id", "");

        const auto cfWorlds = list_worlds_in(cfPath, Origin::CurseForge);
        std::vector<Snapshot> lnWorlds;
        if (!c.launcherInstanceId.empty())
            lnWorlds = list_worlds_in(DataStore::instancesRoot() / c.launcherInstanceId,
                                      Origin::Launcher);

        auto find_by_folder = [](const std::vector<Snapshot>& v,
                                 const std::string& folder) -> const Snapshot* {
            for (const auto& s : v)
                if (_stricmp(s.worldFolder.c_str(), folder.c_str()) == 0) return &s;
            return nullptr;
        };

        for (const auto& w : cfWorlds) {
            const Snapshot* peer = find_by_folder(lnWorlds, w.worldFolder);
            if (!peer) {
                c.onlyInCurseForge.push_back(w);
                continue;
            }
            if (w.lastModified > peer->lastModified) c.newer.push_back(w);
        }
        for (const auto& w : lnWorlds)
            if (!find_by_folder(cfWorlds, w.worldFolder))
                c.onlyInLauncher.push_back(w);

        // Une instance CurseForge sans monde ni correspondance n'apporte rien.
        if (!c.newer.empty() || !c.onlyInCurseForge.empty() ||
            !c.onlyInLauncher.empty())
            out.push_back(std::move(c));
    }
    return out;
}

std::vector<Snapshot> detect_newer_from_curseforge() {
    std::vector<Snapshot> out;
    std::error_code ec;
    for (const auto& c : compare_all()) {
        for (const auto& w : c.newer) {
            // Deja present cote launcher : ce n'est plus un candidat.
            if (!c.launcherInstanceId.empty() &&
                fs::is_directory(DataStore::instancesRoot() / c.launcherInstanceId /
                                     "saves" / w.worldFolder,
                                 ec))
                continue;
            out.push_back(w);
        }
    }
    return out;
}

ImportResult import_world(const Snapshot& w, const std::string& targetInstanceId) {
    ImportResult r;
    std::error_code ec;
    if (!fs::is_directory(w.path, ec)) {
        r.error = "Dossier source introuvable : " + w.path.string();
        return r;
    }
    if (targetInstanceId.empty()) {
        r.error = "Aucune instance de destination.";
        return r;
    }

    const fs::path saves = DataStore::instancesRoot() / targetInstanceId / "saves";
    const fs::path target = saves / w.worldFolder;
    fs::create_directories(saves, ec);

    // Sauvegarde du monde existant AVANT de l'ecraser.
    if (fs::is_directory(target, ec)) {
        const fs::path zip =
            saves / ("_backup_" + w.worldFolder + "_" + iso_stamp() + ".zip");
        if (zip_create_from_dir(target, zip)) {
            r.backup = zip;
        } else {
            // Divergence assumee : le C# continuait meme si l'archive echouait,
            // donc pouvait detruire le monde sans filet. On s'arrete.
            r.error = "Sauvegarde du monde existant impossible : import annulé "
                      "pour ne rien détruire.";
            return r;
        }
        fs::remove_all(target, ec);
        if (ec) {
            r.error = "Impossible de remplacer le monde existant.";
            return r;
        }
    }

    std::string err;
    if (!copy_tree(w.path, target, &err)) {
        r.error = err.empty() ? "Copie du monde impossible." : err;
        return r;
    }
    r.ok = true;
    r.target = target;
    log_line("WorldSync : « " + w.displayName + " » importé depuis CurseForge vers " +
             target.string());
    return r;
}

} // namespace tl::wsync

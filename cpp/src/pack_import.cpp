#include "pack_import.hpp"

#include "curseforge.hpp"
#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"
#include "util_zip.hpp"

#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::packs {

namespace {

// Ecrit un fichier en creant ses dossiers parents. false = echec.
bool write_file(const fs::path& dest, const std::string& data) {
    std::error_code ec;
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

void report(const Progress& p, const std::string& msg) {
    if (p) p(msg);
}

// Garde-fou : un chemin d'entree ne doit pas sortir du dossier de l'instance.
bool safe_relative(const std::string& rel) {
    return !rel.empty() && rel.find("..") == std::string::npos && rel[0] != '/' &&
           rel[0] != '\\' && rel.find(':') == std::string::npos;
}

} // namespace

Kind detect(const fs::path& archive) {
    if (zip_read_entry(archive, "manifest.json")) return Kind::CurseForge;
    if (zip_read_entry(archive, "modrinth.index.json")) return Kind::Modrinth;
    return Kind::Unknown;
}

// ---------------------------------------------------------------------------
// CurseForge (.zip : manifest.json + overrides/)
// ---------------------------------------------------------------------------

Result import_curseforge(const fs::path& zip, const Progress& progress,
                         std::atomic<bool>& cancel) {
    Result res;
    try {
        auto raw = zip_read_entry(zip, "manifest.json");
        if (!raw) {
            res.error =
                "Ce fichier n'est pas un modpack CurseForge valide (manifest.json "
                "absent).";
            return res;
        }
        const json root = json::parse(*raw);

        std::string name = root.value("name", std::string{});
        if (name.empty()) name = zip.stem().string();

        std::string mcVersion, loader = "Vanilla";
        if (auto mc = root.find("minecraft"); mc != root.end() && mc->is_object()) {
            mcVersion = mc->value("version", std::string{});
            if (auto mls = mc->find("modLoaders");
                mls != mc->end() && mls->is_array()) {
                // Fidele au C# : Forge/NeoForge s'imposent (break), Fabric non.
                for (const auto& ml : *mls) {
                    const std::string id = ml.value("id", std::string{});
                    if (id.rfind("forge-", 0) == 0) { loader = "Forge"; break; }
                    if (id.rfind("neoforge-", 0) == 0) { loader = "NeoForge"; break; }
                    if (id.rfind("fabric-", 0) == 0) loader = "Fabric";
                }
            }
        }

        res.instance = make_instance(name, loader, mcVersion);
        res.instance["Description"] =
            "Modpack CurseForge (" + loader + " " + mcVersion + ")";
        const fs::path gameDir =
            DataStore::instancesRoot() / res.instance.value("Id", "");
        std::error_code ec;
        fs::create_directories(gameDir, ec);

        // 1. overrides/ -> racine de l'instance
        report(progress, "Extraction des fichiers du modpack...");
        zip_extract_prefix(zip, "overrides/", gameDir);
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }

        // 2. fichiers listes (projectID + fileID)
        auto files = root.find("files");
        if (files == root.end() || !files->is_array() || files->empty()) return res;

        std::vector<long long> fileIds;
        std::vector<int> projectIds;
        for (const auto& f : *files) {
            projectIds.push_back(f.value("projectID", 0));
            fileIds.push_back(f.value("fileID", 0LL));
        }

        report(progress, "Résolution des mods (" + std::to_string(fileIds.size()) +
                             " fichiers)...");
        const auto details = cf::get_files_by_ids(fileIds, &cancel);
        const auto classes = cf::get_project_classes(projectIds, &cancel);
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }

        std::map<long long, const cf::File*> byId;
        for (const auto& f : details) byId[f.fileId] = &f;

        // Telechargement sequentiel : le C# parallelisait a 4, mais l'API
        // CurseForge limite le debit et un import rate coute un quota.
        for (size_t i = 0; i < fileIds.size(); ++i) {
            if (cancel.load()) {
                res.cancelled = true;
                return res;
            }
            report(progress, "Téléchargement des mods (" + std::to_string(i + 1) +
                                 "/" + std::to_string(fileIds.size()) + ")...");
            auto it = byId.find(fileIds[i]);
            if (it == byId.end()) {
                ++res.failed; // fichier introuvable : on saute, comme le C#
                continue;
            }
            const auto cls = classes.find(projectIds[i]);
            const int classId = cls == classes.end() ? cf::kClassMods : cls->second;
            const char* sub = classId == cf::kClassResourcePacks ? "resourcepacks"
                              : classId == cf::kClassShaders     ? "shaderpacks"
                                                                 : "mods";
            if (cf::download_file(*it->second, gameDir / sub, &cancel).empty()) {
                ++res.failed;
                log_line("modpack CF : échec du fichier " +
                         std::to_string(fileIds[i]));
            } else {
                ++res.downloaded;
            }
        }
    } catch (const std::exception& ex) {
        res.error = ex.what();
    }
    return res;
}

// ---------------------------------------------------------------------------
// Modrinth (.mrpack : modrinth.index.json + overrides/)
// ---------------------------------------------------------------------------

Result import_modrinth(const fs::path& mrpack, const Progress& progress,
                       std::atomic<bool>& cancel) {
    Result res;
    try {
        auto raw = zip_read_entry(mrpack, "modrinth.index.json");
        if (!raw) {
            res.error =
                "Ce fichier n'est pas un modpack Modrinth valide "
                "(modrinth.index.json absent).";
            return res;
        }
        const json root = json::parse(*raw);

        std::string name = root.value("name", std::string{});
        if (name.empty()) name = mrpack.stem().string();

        // Le C# lit « gameVersion » ; les .mrpack recents mettent la version
        // dans dependencies.minecraft — on accepte les deux.
        std::string gameVersion = root.value("gameVersion", std::string{});
        std::string loader = "Vanilla";
        if (auto deps = root.find("dependencies");
            deps != root.end() && deps->is_object()) {
            for (auto it = deps->begin(); it != deps->end(); ++it) {
                if (it.key() == "fabric-loader") { loader = "Fabric"; break; }
                if (it.key() == "forge") { loader = "Forge"; break; }
                if (it.key() == "neoforge") { loader = "NeoForge"; break; }
            }
            if (gameVersion.empty()) gameVersion = deps->value("minecraft", "");
        }

        res.instance = make_instance(name, loader, gameVersion);
        res.instance["Description"] =
            "Modpack Modrinth (" + loader + " " + gameVersion + ")";
        const fs::path gameDir =
            DataStore::instancesRoot() / res.instance.value("Id", "");
        std::error_code ec;
        fs::create_directories(gameDir, ec);

        report(progress, "Extraction des fichiers du modpack...");
        zip_extract_prefix(mrpack, "overrides/", gameDir);
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }

        auto files = root.find("files");
        if (files == root.end() || !files->is_array()) return res;

        const int total = static_cast<int>(files->size());
        int done = 0;
        for (const auto& f : *files) {
            if (cancel.load()) {
                res.cancelled = true;
                return res;
            }
            ++done;
            report(progress, "Mods et fichiers (" + std::to_string(done) + "/" +
                                 std::to_string(total) + ")...");
            const std::string rel = f.value("path", std::string{});
            if (!safe_relative(rel)) {
                ++res.failed;
                continue;
            }
            auto dl = f.find("downloads");
            if (dl == f.end() || !dl->is_array() || dl->empty()) {
                ++res.failed;
                continue;
            }
            const std::string url = dl->front().get<std::string>();
            auto r = http::get_response(url, {}, &cancel);
            if (!r || r->status != 200 || !write_file(gameDir / rel, r->body)) {
                ++res.failed;
                log_line("mrpack : échec de " + rel);
                continue;
            }
            ++res.downloaded;
        }
    } catch (const std::exception& ex) {
        res.error = ex.what();
    }
    return res;
}

Result import_any(const fs::path& archive, const Progress& progress,
                  std::atomic<bool>& cancel) {
    switch (detect(archive)) {
    case Kind::CurseForge: return import_curseforge(archive, progress, cancel);
    case Kind::Modrinth: return import_modrinth(archive, progress, cancel);
    default: {
        Result res;
        res.error =
            "Format non reconnu : ni manifest.json (CurseForge) ni "
            "modrinth.index.json (Modrinth) dans l'archive.";
        return res;
    }
    }
}

} // namespace tl::packs

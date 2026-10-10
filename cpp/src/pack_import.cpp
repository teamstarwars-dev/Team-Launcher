#include "pack_import.hpp"

#include "curseforge.hpp"
#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"
#include "modmeta.hpp"       // detect_folder : estimation d'apres les mods
#include "util_zip.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

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
// safe_relative vivait ici, en double avec le contrôle de util_zip.cpp.
// Celui-ci etait le plus strict des deux — il rejetait « : », donc les
// lettres de lecteur — et c'est l'AUTRE qui avait le defaut. Deux
// implementations separees du meme controle, c'est une occasion de plus
// qu'une seule se trompe : il n'en reste qu'une, safe_join, et elle
// verifie le chemin OBTENU plutot que la forme du nom.

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
            const auto dest = safe_join(gameDir, rel);
            if (!dest) {
                ++res.failed;
                log_line("mrpack : chemin refusé « " + rel + " »");
                continue;
            }
            auto dl = f.find("downloads");
            if (dl == f.end() || !dl->is_array() || dl->empty()) {
                ++res.failed;
                continue;
            }
            const std::string url = dl->front().get<std::string>();
            auto r = http::get_response(url, {}, &cancel);
            if (!r || r->status != 200 || !write_file(*dest, r->body)) {
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

// ---------------------------------------------------------------------------
// Dossier d'instance existant
// ---------------------------------------------------------------------------

namespace {

std::optional<json> read_json(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream ss;
    ss << in.rdbuf();
    json j = json::parse(ss.str(), nullptr, false);
    if (j.is_discarded() || !j.is_object()) return std::nullopt;
    return j;
}

std::string str_of(const json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

// « Vanilla » si le texte ne nomme aucun chargeur connu.
std::string loader_label(const std::string& s) {
    const auto l = modmeta::loader_from_string(s);
    return l == modmeta::Loader::Unknown ? std::string("Vanilla")
                                         : std::string(modmeta::loader_name(l));
}

// Premiere version de Minecraft concrete dans une contrainte de mod :
// « [1.20.1,1.21) » -> 1.20.1, « ~1.20.1 » -> 1.20.1, « >=1.19 » -> 1.19.
// Vide si la contrainte ne cite aucune version (« * », « [47,) »…).
std::string first_mc_version(const std::string& range) {
    for (std::size_t i = 0; i < range.size(); ++i) {
        if (range[i] != '1' || (i > 0 && std::isdigit(static_cast<unsigned char>(range[i - 1]))))
            continue;
        std::size_t j = i + 1;
        if (j >= range.size() || range[j] != '.') continue;
        std::string v = "1.";
        ++j;
        while (j < range.size() &&
               (std::isdigit(static_cast<unsigned char>(range[j])) || range[j] == '.'))
            v.push_back(range[j++]);
        while (!v.empty() && v.back() == '.') v.pop_back();
        if (v.size() > 2) return v;
    }
    return {};
}

} // namespace

FolderInfo detect_folder(const fs::path& dir) {
    FolderInfo f;
    f.gameDir = dir;
    std::error_code ec;

    // 1. CurseForge : minecraftinstance.json, a la racine du jeu.
    //    baseModLoader.name vaut « forge-14.23.5.2859 », « fabric-0.16.9-1.21.1 »…
    if (const auto j = read_json(dir / "minecraftinstance.json")) {
        f.source = "CurseForge";
        f.name = str_of(*j, "name");
        if (const auto v = str_of(*j, "gameVersion"); !v.empty()) f.mcVersion = v;
        if (const auto b = j->find("baseModLoader"); b != j->end() && b->is_object())
            f.loader = loader_label(str_of(*b, "name"));
        return f;
    }

    // 2. Prism Launcher / MultiMC : mmc-pack.json liste les composants, le
    //    nom est dans instance.cfg, et les fichiers du jeu vivent dans un
    //    sous-dossier — copier la racine aurait range mods/ un niveau trop
    //    bas, ou Minecraft ne le trouverait pas.
    if (const auto j = read_json(dir / "mmc-pack.json")) {
        f.source = "Prism";
        if (const auto c = j->find("components"); c != j->end() && c->is_array())
            for (const auto& comp : *c) {
                if (!comp.is_object()) continue;
                const std::string uid = str_of(comp, "uid");
                const std::string ver = str_of(comp, "version");
                if (uid == "net.minecraft" && !ver.empty()) f.mcVersion = ver;
                else if (uid == "net.minecraftforge") f.loader = "Forge";
                else if (uid == "net.neoforged") f.loader = "NeoForge";
                else if (uid == "net.fabricmc.fabric-loader") f.loader = "Fabric";
                else if (uid == "org.quiltmc.quilt-loader") f.loader = "Quilt";
            }
        std::ifstream cfg(dir / "instance.cfg");
        for (std::string line; std::getline(cfg, line);)
            if (line.rfind("name=", 0) == 0) {
                f.name = line.substr(5);
                if (!f.name.empty() && f.name.back() == '\r') f.name.pop_back();
                break;
            }
        for (const char* sub : {".minecraft", "minecraft"})
            if (fs::is_directory(dir / sub, ec)) {
                f.gameDir = dir / sub;
                break;
            }
        return f;
    }

    // 3. Modrinth App : profile.json (format des versions 0.x).
    if (const auto j = read_json(dir / "profile.json")) {
        if (const auto m = j->find("metadata"); m != j->end() && m->is_object()) {
            f.source = "Modrinth";
            f.name = str_of(*m, "name");
            if (const auto v = str_of(*m, "game_version"); !v.empty()) f.mcVersion = v;
            f.loader = loader_label(str_of(*m, "loader"));
            return f;
        }
    }

    // 4. Aucun manifeste : estimation d'apres les mods eux-memes. Le
    //    chargeur majoritaire l'emporte, et la version de Minecraft la plus
    //    citee en borne basse des contraintes. C'est une ESTIMATION, signalee
    //    comme telle — mais elle vaut mieux que Vanilla pour un dossier qui
    //    contient des dizaines de mods Forge.
    const auto mods = modmeta::read_dir(f.gameDir / "mods");
    std::map<std::string, int> loaders, versions;
    for (const auto& m : mods) {
        if (m.disabled || m.loader == modmeta::Loader::Unknown) continue;
        ++loaders[modmeta::loader_name(m.loader)];
        if (const auto v = first_mc_version(m.mcRange); !v.empty()) ++versions[v];
    }
    auto top = [](const std::map<std::string, int>& m) {
        std::string best;
        int n = 0;
        for (const auto& [k, c] : m)
            if (c > n) { best = k; n = c; }
        return best;
    };
    if (const auto l = top(loaders); !l.empty()) {
        f.source = "mods";
        f.loader = l;
        if (const auto v = top(versions); !v.empty()) f.mcVersion = v;
    }
    return f;
}

} // namespace tl::packs

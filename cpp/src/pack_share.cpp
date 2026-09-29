#include "pack_share.hpp"

#include "curseforge.hpp" // sanitize()
#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"
#include "modrinth.hpp"
#include "util_hash.hpp"
#include "util_str.hpp" // strCaseCmp

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::share {

namespace {

void report(const Progress& p, const std::string& s) {
    if (p) p(s);
}

bool iends_with(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    if (s.size() < n) return false;
    return tl::strCaseCmp(s.c_str() + (s.size() - n), suffix) == 0;
}

// Liste les fichiers d'un sous-dossier d'instance (non recursif, comme le C#
// pour mods/shaderpacks).
std::vector<fs::path> list_files(const fs::path& dir, const char* ext) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (ext && !iends_with(e.path().filename().string(), ext)) continue;
        out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

// Empreintes + resolution Modrinth pour un sous-dossier.
json scan_and_resolve(const fs::path& instDir, const char* subDir, const char* ext,
                      const Progress& progress, std::atomic<bool>& cancel,
                      int* recognized) {
    json items = json::array();
    *recognized = 0;

    const auto files = list_files(instDir / subDir, ext);
    if (files.empty()) return items;

    // 1. SHA1 de chaque fichier
    std::vector<std::string> hashes;
    std::vector<fs::path> kept;
    for (size_t i = 0; i < files.size(); ++i) {
        if (cancel.load()) return items;
        report(progress, std::string("Empreinte ") + subDir + " (" +
                             std::to_string(i + 1) + "/" +
                             std::to_string(files.size()) + ")...");
        auto h = sha1_hex(files[i]);
        if (!h) continue;
        hashes.push_back(*h);
        kept.push_back(files[i]);
    }
    if (hashes.empty()) return items;

    // 2. Resolution groupee sur Modrinth
    report(progress, std::string("Recherche des ") + subDir + " sur Modrinth...");
    const auto matches = mr::version_files(hashes, &cancel);

    // 3. Construction de la liste
    std::error_code ec;
    for (size_t i = 0; i < kept.size(); ++i) {
        const std::string sha1 = hashes[i];
        json item = {{"ProjectId", ""},
                     {"Filename", kept[i].filename().string()},
                     {"Url", ""},
                     {"Sha1", sha1},
                     {"Size", static_cast<long long>(fs::file_size(kept[i], ec))}};
        if (auto it = matches.find(sha1); it != matches.end()) {
            item["ProjectId"] = it->second.projectId;
            item["Url"] = it->second.url;
            if (!it->second.url.empty()) ++(*recognized);
        }
        items.push_back(std::move(item));
    }
    return items;
}

// Fichiers listes pour information (configs, mondes, resource packs) : le C#
// ne les transporte pas, il les enumere seulement dans le descriptif.
json scan_files(const fs::path& instDir, const char* subDir, bool recursive) {
    json out = json::array();
    const fs::path dir = instDir / subDir;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;

    auto add = [&](const fs::path& p) {
        const std::string rel = fs::relative(p, dir, ec).generic_string();
        if (ec || rel.empty()) return;
        out.push_back({{"Path", rel},
                       {"Sha1", sha1_hex(p).value_or("")},
                       {"Size", static_cast<long long>(fs::file_size(p, ec))}});
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(dir, ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) add(it->path());
        }
    } else {
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            if (e.is_regular_file(ec)) add(e.path());
        }
    }
    return out;
}

int download_items(const json& items, const fs::path& destDir,
                   const char* label, const Progress& progress,
                   std::atomic<bool>& cancel, int* downloaded) {
    int failed = 0;
    std::error_code ec;
    fs::create_directories(destDir, ec);
    const size_t total = items.size();
    size_t i = 0;
    for (const auto& it : items) {
        if (cancel.load()) break;
        ++i;
        const std::string url = it.value("Url", std::string{});
        const std::string name = it.value("Filename", std::string{});
        report(progress, std::string(label) + " (" + std::to_string(i) + "/" +
                             std::to_string(total) + ")...");
        if (url.empty() || name.empty()) {
            // Fichier non reconnu par Modrinth : rien a telecharger.
            ++failed;
            continue;
        }
        auto r = http::get_response(url, {}, &cancel);
        if (!r || r->status != 200 || r->body.empty()) {
            ++failed;
            log_line("Pack partagé : échec du téléchargement de " + name);
            continue;
        }
        std::ofstream out(destDir / cf::sanitize(name), std::ios::binary | std::ios::trunc);
        if (!out) {
            ++failed;
            continue;
        }
        out.write(r->body.data(), static_cast<std::streamsize>(r->body.size()));
        if (!out) {
            ++failed;
            continue;
        }
        ++(*downloaded);
    }
    return failed;
}

} // namespace

ExportResult export_pack(const json& inst, const Progress& progress,
                         std::atomic<bool>& cancel) {
    ExportResult res;
    try {
        const std::string id = inst.value("Id", std::string{});
        if (id.empty()) {
            res.error = "Instance invalide.";
            return res;
        }
        const fs::path dir = DataStore::instancesRoot() / id;

        report(progress, "Analyse des mods...");
        json mods = scan_and_resolve(dir, "mods", ".jar", progress, cancel,
                                     &res.stats.recognizedMods);
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }
        report(progress, "Analyse des shaders...");
        json shaders = scan_and_resolve(dir, "shaderpacks", ".zip", progress, cancel,
                                        &res.stats.recognizedShaders);
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }
        res.stats.mods = static_cast<int>(mods.size());
        res.stats.shaders = static_cast<int>(shaders.size());

        report(progress, "Analyse des resource packs...");
        json resourcePacks = scan_files(dir, "resourcepacks", false);
        report(progress, "Analyse des configs...");
        json configs = scan_files(dir, "config", false);
        report(progress, "Analyse des mondes...");
        json worlds = scan_files(dir, "saves", true);

        // Memes noms de champs que le C# (compat descriptifs v5).
        res.pack = {{"Format", kFormatId},
                    {"Name", inst.value("Name", std::string{})},
                    {"Description", inst.value("Description", std::string{})},
                    {"Loader", inst.value("Loader", std::string{})},
                    {"McVersion", inst.value("McVersion", std::string{})},
                    {"Mods", std::move(mods)},
                    {"Shaders", std::move(shaders)},
                    {"ResourcePacks", std::move(resourcePacks)},
                    {"Configs", std::move(configs)},
                    {"Worlds", std::move(worlds)}};
    } catch (const std::exception& ex) {
        res.error = ex.what();
    }
    return res;
}

std::string serialize(const json& pack) { return pack.dump(2); }

bool looks_like_pack(const std::string& text) {
    if (text.find(kFormatId) != std::string::npos) return true;
    // Tolerant : un descriptif edite a la main peut avoir perdu le champ
    // Format, on accepte un objet portant Mods ou Shaders.
    try {
        const json j = json::parse(text);
        return j.is_object() && (j.contains("Mods") || j.contains("Shaders"));
    } catch (const json::exception&) {
        return false;
    }
}

ImportResult import_pack(const std::string& text, const Progress& progress,
                         std::atomic<bool>& cancel) {
    ImportResult res;
    json pack;
    try {
        pack = json::parse(text);
        if (!pack.is_object()) throw std::runtime_error("objet attendu");
    } catch (const std::exception&) {
        res.error = "Ce texte n'est pas un pack Team Launcher valide.";
        return res;
    }

    // Lecture insensible a la casse des cles (le C# desserialisait avec
    // PropertyNameCaseInsensitive).
    auto field = [&pack](const char* name) -> const json* {
        for (auto it = pack.begin(); it != pack.end(); ++it)
            if (tl::strCaseCmp(it.key().c_str(), name) == 0) return &it.value();
        return nullptr;
    };
    auto arr = [&](const char* name) -> json {
        const json* v = field(name);
        return (v && v->is_array()) ? *v : json::array();
    };
    auto str = [&](const char* name) -> std::string {
        const json* v = field(name);
        return (v && v->is_string()) ? v->get<std::string>() : std::string{};
    };

    const json mods = arr("Mods");
    const json shaders = arr("Shaders");
    if (mods.empty() && shaders.empty()) {
        res.error = "Ce pack ne contient ni mod ni shader.";
        return res;
    }

    std::string name = str("Name");
    if (name.empty()) name = "Pack partagé";
    std::string loader = str("Loader");
    if (loader.empty()) loader = "Vanilla";
    std::string mcVersion = str("McVersion");
    if (mcVersion.empty()) mcVersion = "latest";
    std::string desc = str("Description");
    if (desc.empty()) desc = "Pack partagé (" + loader + " " + mcVersion + ")";

    res.instance = make_instance(name, loader, mcVersion);
    res.instance["Description"] = desc;
    const fs::path dir = DataStore::instancesRoot() / res.instance.value("Id", "");

    if (!mods.empty())
        res.failed += download_items(mods, dir / "mods", "Mods", progress, cancel,
                                     &res.downloaded);
    if (!cancel.load() && !shaders.empty())
        res.failed += download_items(shaders, dir / "shaderpacks", "Shaders",
                                     progress, cancel, &res.downloaded);
    if (cancel.load()) res.cancelled = true;
    return res;
}

} // namespace tl::share

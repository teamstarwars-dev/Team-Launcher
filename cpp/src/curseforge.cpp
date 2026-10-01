#include "curseforge.hpp"

#include "netcache.hpp"

#include "datastore.hpp"
#include "http_win.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::cf {

namespace {

constexpr int kMinecraftGameId = 432;
constexpr const char* kBase = "https://api.curseforge.com/v1/";

const char* const kKnownLoaders[] = {"forge", "fabric", "neoforge", "quilt"};

bool is_known_loader(const std::string& v) {
    for (const char* l : kKnownLoaders)
        if (v == l) return true;
    return false;
}

std::string headers() {
    return "x-api-key: " + api_key() + "\r\nAccept: application/json";
}

void ensure_key() {
    if (!has_key()) throw std::runtime_error(missing_key_message());
}

std::string truncate(const std::string& s, size_t n) {
    return s.size() > n ? s.substr(0, n) : s;
}

json parse_or_throw(const std::optional<http::Response>& r, const std::string& what) {
    if (!r) throw std::runtime_error("Échec réseau vers " + what + ".");
    if (r->status == 401 || r->status == 403)
        throw std::runtime_error(
            "CurseForge a refusé la clé API (HTTP " + std::to_string(r->status) +
            ").\nVérifie-la dans Paramètres > Intégrations.");
    if (r->status != 200)
        throw std::runtime_error("HTTP " + std::to_string(r->status) + " depuis " +
                                 what + "\nRéponse : " + truncate(r->body, 300));
    try {
        return json::parse(r->body);
    } catch (const json::exception&) {
        throw std::runtime_error("Réponse CurseForge illisible depuis " + what + ".");
    }
}

json api_get(const std::string& path, const std::atomic<bool>* cancel) {
    ensure_key();
    const std::string url = kBase + path;

    // Cache disque, comme cote Modrinth : memes requetes repetees a chaque
    // navigation, et CurseForge limite le debit par cle API. On ne met en
    // cache que les 200 — figer un 401 « cle refusee » pendant un quart
    // d'heure empecherait de voir l'effet d'une cle corrigee.
    //
    // Les erreurs doivent rester des erreurs PARLANTES : en cas d'echec
    // sans cache, on refait l'appel pour que parse_or_throw produise le
    // message exact (cle refusee, HTTP 500, corps tronque...).
    const auto res = netcache::get(
        url, 15 * 60, [&](const std::string& u) -> std::optional<std::string> {
            auto r = http::get_response(u, headers(), cancel);
            if (!r || r->status != 200) return std::nullopt;
            return r->body;
        });
    if (!res.ok)
        return parse_or_throw(http::get_response(url, headers(), cancel), path);
    try {
        return json::parse(res.body);
    } catch (const json::exception&) {
        netcache::drop(url); // entree abimee : on repart du reseau ensuite
        throw std::runtime_error("Réponse CurseForge illisible depuis " + path + ".");
    }
}

json api_post(const std::string& path, const json& body,
              const std::atomic<bool>* cancel) {
    ensure_key();
    return parse_or_throw(
        http::post_string(kBase + path, body.dump(), "application/json", headers(),
                          cancel),
        path);
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out.push_back(static_cast<char>(ch));
        else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0xF]);
        }
    }
    return out;
}

// Fidele a ParseFile C# : les entrees « loader » et « Java*/Vanilla » sont
// retirees de gameVersions, les loaders connus en sont extraits.
File parse_file(const json& f) {
    File out;
    out.fileId = f.value("id", 0LL);
    out.modId = f.value("modId", 0);
    out.fileName = f.value("fileName", std::string{});
    out.displayName = f.value("displayName", std::string{});
    if (auto it = f.find("downloadUrl"); it != f.end() && it->is_string())
        out.downloadUrl = it->get<std::string>();
    if (auto it = f.find("gameVersions"); it != f.end() && it->is_array()) {
        for (const auto& g : *it) {
            if (!g.is_string()) continue;
            const std::string v = g.get<std::string>();
            if (is_known_loader(v)) {
                out.loaders.push_back(v);
            } else if (v.rfind("Java", 0) != 0 && v != "Vanilla") {
                out.gameVersions.push_back(v);
            }
        }
    }
    out.fileDate = f.value("fileDate", std::string{});
    return out;
}

} // namespace

std::string api_key() {
    const std::string& k = DataStore::settings.curseForgeApiKey;
    if (!k.empty()) return k;
    if (const char* env = std::getenv("CURSEFORGE_API_KEY")) return env;
    return {};
}

bool has_key() { return !api_key().empty(); }

const char* missing_key_message() {
    // Ce message ne s'adresse PLUS a l'utilisateur ordinaire : les builds
    // officiels embarquent une cle. Le voir signifie qu'on utilise une
    // compilation faite sans — un clone du depot, par exemple. Le texte
    // dit donc d'abord ce qui marche quand meme, avant de demander quoi
    // que ce soit.
    return "Cette version a été compilée sans clé API CurseForge.\n\n"
           "Modrinth reste entièrement accessible : la majorité des mods "
           "et modpacks s'y trouvent aussi.\n\n"
           "Pour activer CurseForge :\n"
           "1. Crée un compte sur console.curseforge.com\n"
           "2. Génère une clé API (gratuite, immédiate)\n"
           "3. Colle-la dans Paramètres > Intégrations > Clé API CurseForge";
}

const char* loader_name(int modLoader) {
    switch (modLoader) {
    case 1: return "forge";
    case 4: return "fabric";
    case 5: return "quilt";
    case 6: return "neoforge";
    default: return "";
    }
}

std::vector<Hit> search(const std::string& query, int classId,
                        const std::atomic<bool>* cancel) {
    std::string path = "mods/search?gameId=" + std::to_string(kMinecraftGameId) +
                       "&classId=" + std::to_string(classId) +
                       "&sortField=2&sortOrder=desc&pageSize=25";
    if (!query.empty()) path += "&searchFilter=" + url_encode(query);

    const json root = api_get(path, cancel);
    std::vector<Hit> out;
    auto data = root.find("data");
    if (data == root.end() || !data->is_array()) return out;

    for (const auto& m : *data) {
        Hit h;
        h.projectId = m.value("id", 0);
        h.slug = m.value("slug", std::string{});
        h.title = m.value("name", std::string{});
        h.downloads = static_cast<long long>(m.value("downloadCount", 0.0));
        h.description = m.value("summary", std::string{});
        // loaders dedoublonnes, dans l'ordre (C# Distinct() preserve l'ordre)
        if (auto idx = m.find("latestFilesIndexes");
            idx != m.end() && idx->is_array()) {
            for (const auto& i : *idx) {
                const char* l = loader_name(i.value("modLoader", 0));
                if (!*l) continue;
                const std::string s = l;
                if (h.loaders.find(s) != std::string::npos) continue;
                if (!h.loaders.empty()) h.loaders.push_back(' ');
                h.loaders += s;
            }
        }
        if (auto lg = m.find("logo"); lg != m.end() && lg->is_object())
            if (auto tu = lg->find("thumbnailUrl"); tu != lg->end() && tu->is_string())
                h.iconUrl = tu->get<std::string>();
        out.push_back(std::move(h));
    }
    return out;
}

std::vector<File> get_files(int projectId, const std::atomic<bool>* cancel) {
    const json root = api_get("mods/" + std::to_string(projectId) + "/files", cancel);
    std::vector<File> out;
    if (auto data = root.find("data"); data != root.end() && data->is_array())
        for (const auto& f : *data) out.push_back(parse_file(f));
    // ISO 8601 : l'ordre lexicographique est l'ordre chronologique
    std::stable_sort(out.begin(), out.end(),
                     [](const File& a, const File& b) {
                         return a.fileDate > b.fileDate;
                     });
    return out;
}

std::vector<File> get_files_by_ids(const std::vector<long long>& fileIds,
                                   const std::atomic<bool>* cancel) {
    std::vector<File> out;
    for (size_t i = 0; i < fileIds.size(); i += 64) {
        const size_t n = (std::min)(size_t{64}, fileIds.size() - i);
        json batch = json::array();
        for (size_t k = 0; k < n; ++k) batch.push_back(fileIds[i + k]);
        const json root = api_post("mods/files", {{"fileIds", batch}}, cancel);
        if (auto data = root.find("data"); data != root.end() && data->is_array())
            for (const auto& f : *data) out.push_back(parse_file(f));
    }
    return out;
}

std::map<int, int> get_project_classes(const std::vector<int>& projectIds,
                                       const std::atomic<bool>* cancel) {
    // Dedoublonnage (C# Distinct()) en preservant l'ordre.
    std::vector<int> ids;
    for (int id : projectIds)
        if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);

    std::map<int, int> map;
    for (size_t i = 0; i < ids.size(); i += 64) {
        const size_t n = (std::min)(size_t{64}, ids.size() - i);
        json batch = json::array();
        for (size_t k = 0; k < n; ++k) batch.push_back(ids[i + k]);
        const json root = api_post("mods", {{"modIds", batch}}, cancel);
        if (auto data = root.find("data"); data != root.end() && data->is_array())
            for (const auto& m : *data) {
                const int id = m.value("id", 0);
                auto c = m.find("classId");
                map[id] = (c != m.end() && c->is_number()) ? c->get<int>() : kClassMods;
            }
    }
    return map;
}

std::string sanitize(const std::string& name) {
    std::string out = name;
    for (char& c : out)
        if (c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
            c == '|' || c == '?' || c == '*' || static_cast<unsigned char>(c) < 32)
            c = '_';
    return out;
}

std::string download_file(const File& f, const fs::path& destDir,
                          const std::atomic<bool>* cancel) {
    if (!has_key()) return {};
    std::error_code ec;
    fs::create_directories(destDir, ec);
    const fs::path dest = destDir / sanitize(f.fileName);

    // Depuis juillet 2026 le CDN exige aussi la cle (header x-api-key).
    const std::string url =
        !f.downloadUrl.empty()
            ? f.downloadUrl
            : "https://www.curseforge.com/api/v1/mods/" + std::to_string(f.modId) +
                  "/files/" + std::to_string(f.fileId) + "/download";

    auto r = http::get_response(url, "x-api-key: " + api_key(), cancel);
    if (!r || r->status != 200 || r->body.empty()) return {};
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) return {};
    out.write(r->body.data(), static_cast<std::streamsize>(r->body.size()));
    if (!out) return {};
    return dest.string();
}

long long murmur2(const std::string& data, unsigned seed) {
    unsigned len = static_cast<unsigned>(data.size());
    if (len == 0) return 0;
    const unsigned m = 0x5bd1e995u;
    const int r = 24;
    unsigned h = seed ^ len;
    size_t i = 0;
    while (len >= 4) {
        unsigned k = static_cast<unsigned char>(data[i]) |
                     (static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8) |
                     (static_cast<unsigned>(static_cast<unsigned char>(data[i + 2])) << 16) |
                     (static_cast<unsigned>(static_cast<unsigned char>(data[i + 3])) << 24);
        k *= m;
        k ^= k >> r;
        k *= m;
        h *= m;
        h ^= k;
        i += 4;
        len -= 4;
    }
    switch (len) {
    case 3: h ^= static_cast<unsigned>(static_cast<unsigned char>(data[i + 2])) << 16;
        [[fallthrough]];
    case 2: h ^= static_cast<unsigned>(static_cast<unsigned char>(data[i + 1])) << 8;
        [[fallthrough]];
    case 1:
        h ^= static_cast<unsigned char>(data[i]);
        h *= m;
        break;
    default: break;
    }
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return static_cast<long long>(h);
}

long long compute_fingerprint(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return 0;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string raw = ss.str();
    // Normalisation CurseForge : \t (9), \n (10) et \r (13) retires.
    std::string norm;
    norm.reserve(raw.size());
    for (char c : raw)
        if (c != 9 && c != 10 && c != 13) norm.push_back(c);
    return murmur2(norm);
}

} // namespace tl::cf

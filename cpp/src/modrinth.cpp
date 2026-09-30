#include "modrinth.hpp"

#include "netcache.hpp"

#include "curseforge.hpp" // sanitize()
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::mr {

namespace {

const char* const kKnownLoaders[] = {"forge", "fabric", "quilt", "neoforge", "rift"};

bool is_known_loader(const std::string& v) {
    for (const char* l : kKnownLoaders)
        if (v == l) return true;
    return false;
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

json get_json(const std::string& url, const std::atomic<bool>* cancel) {
    // Metadonnees publiques : elles passent par le cache disque. Le meme
    // catalogue et les memes fiches sont redemandes a chaque retour sur la
    // page, et Modrinth limite le debit. 15 minutes est un compromis : assez
    // pour une session de navigation, assez court pour qu'une nouvelle
    // version de mod apparaisse sans avoir a vider le cache.
    //
    // Interet secondaire : quand le reseau tombe, `netcache::get` sert la
    // derniere copie connue plutot que de faire echouer la page.
    const auto res = netcache::get(
        url, 15 * 60, [&](const std::string& u) -> std::optional<std::string> {
            auto r = http::get_response(u, "Accept: application/json", cancel);
            // Seul un 200 merite d'etre mis en cache : mettre un 404 ou un
            // 503 en cache le figerait pour un quart d'heure.
            if (!r || r->status != 200) return std::nullopt;
            return r->body;
        });
    if (!res.ok)
        throw std::runtime_error("Échec réseau vers api.modrinth.com.");
    try {
        return json::parse(res.body);
    } catch (const json::exception&) {
        // Une entree de cache abimee ne doit pas condamner la page : on la
        // jette pour que le prochain appel reparte du reseau.
        netcache::drop(url);
        throw std::runtime_error("Réponse Modrinth illisible.");
    }
}

} // namespace

std::vector<Hit> search(const std::string& query, const std::string& projectType,
                        const std::atomic<bool>* cancel) {
    // facets=[["project_type:<type>"]] — encode comme le C#.
    const std::string url =
        "https://api.modrinth.com/v2/search?limit=25&query=" + url_encode(query) +
        "&facets=%5B%5B%22project_type%3A" + url_encode(projectType) + "%22%5D%5D";

    const json root = get_json(url, cancel);
    std::vector<Hit> out;
    auto hits = root.find("hits");
    if (hits == root.end() || !hits->is_array()) return out;

    for (const auto& h : *hits) {
        Hit e;
        e.title = h.value("title", std::string{});
        e.slug = h.value("slug", std::string{});
        e.type = h.value("project_type", std::string{});
        e.downloads = h.value("downloads", 0LL);
        e.description = h.value("description", std::string{});
        if (auto cats = h.find("categories"); cats != h.end() && cats->is_array())
            for (const auto& c : *cats) {
                if (!c.is_string()) continue;
                const std::string v = c.get<std::string>();
                if (!is_known_loader(v)) continue;
                if (!e.loaders.empty()) e.loaders.push_back(' ');
                e.loaders += v;
            }
        if (auto ic = h.find("icon_url"); ic != h.end() && ic->is_string())
            e.iconUrl = ic->get<std::string>();
        out.push_back(std::move(e));
    }
    return out;
}

namespace {

// Extrait projet + fichier principal d'un objet « version » Modrinth.
FileMatch parse_version(const json& v) {
    FileMatch m;
    m.projectId = v.value("project_id", std::string{});
    auto files = v.find("files");
    if (files != v.end() && files->is_array() && !files->empty()) {
        const json* chosen = nullptr;
        for (const auto& f : *files)
            if (f.value("primary", false)) {
                chosen = &f;
                break;
            }
        if (!chosen) chosen = &files->front();
        m.url = chosen->value("url", std::string{});
        m.filename = chosen->value("filename", std::string{});
    }
    return m;
}

} // namespace

std::map<std::string, FileMatch> version_files(const std::vector<std::string>& sha1s,
                                               const std::atomic<bool>* cancel) {
    std::map<std::string, FileMatch> out;
    if (sha1s.empty()) return out;

    // Cloudflare bloque les POST vers /v2/version_files depuis certains
    // reseaux (403 + page de blocage, quel que soit le User-Agent) alors que
    // les GET passent. On tente donc le lot, et on retombe sur le point
    // d'entree unitaire GET /v2/version_file/<sha1> si le lot est refuse.
    bool batchWorks = true;

    // L'API accepte un lot ; on decoupe par prudence sur les grosses instances.
    for (size_t i = 0; i < sha1s.size(); i += 200) {
        const size_t n = (std::min)(size_t{200}, sha1s.size() - i);
        json hashes = json::array();
        for (size_t k = 0; k < n; ++k) hashes.push_back(sha1s[i + k]);
        const json body = {{"hashes", hashes}, {"algorithm", "sha1"}};

        auto r = http::post_string("https://api.modrinth.com/v2/version_files",
                                   body.dump(), "application/json",
                                   "Accept: application/json", cancel);
        // Echec = aucun fichier reconnu, pas une erreur bloquante : le pack
        // partira avec les entrees non resolues (l'ami devra les fournir).
        // On journalise quand meme : sans cela un refus de l'API passe pour
        // « aucun mod reconnu », ce qui envoie chercher le probleme ailleurs.
        if (!r || r->status != 200) {
            log_line("Modrinth version_files (lot) : HTTP " +
                     std::to_string(r ? r->status : 0) +
                     " — bascule sur les requêtes unitaires.");
            batchWorks = false;
            break;
        }
        json doc;
        try {
            doc = json::parse(r->body);
        } catch (const json::exception&) {
            log_line("Modrinth version_files (lot) : réponse illisible.");
            batchWorks = false;
            break;
        }
        if (!doc.is_object()) {
            batchWorks = false;
            break;
        }

        for (auto it = doc.begin(); it != doc.end(); ++it) {
            if (!it.value().is_object()) continue;
            // Cles en minuscules, comme le C#.
            std::string key = it.key();
            for (char& c : key) c = static_cast<char>(std::tolower((unsigned char)c));
            out[key] = parse_version(it.value());
        }
    }
    if (batchWorks) return out;

    // --- Repli : une requete GET par empreinte (404 = inconnue) ---
    out.clear();
    for (const std::string& raw : sha1s) {
        if (cancel && cancel->load(std::memory_order_relaxed)) break;
        std::string sha1 = raw;
        for (char& c : sha1) c = static_cast<char>(std::tolower((unsigned char)c));
        auto r = http::get_response("https://api.modrinth.com/v2/version_file/" +
                                        url_encode(sha1) + "?algorithm=sha1",
                                    "Accept: application/json", cancel);
        if (!r || r->status != 200) continue; // 404 : fichier non publie sur Modrinth
        try {
            const json v = json::parse(r->body);
            if (v.is_object()) out[sha1] = parse_version(v);
        } catch (const json::exception&) {
            // reponse inattendue : on ignore cette empreinte
        }
    }
    return out;
}

std::string download_project_file(const std::string& slug, const fs::path& destDir,
                                  const std::string& loader,
                                  const std::string& mcVersion,
                                  const std::atomic<bool>* cancel) {
    std::string url =
        "https://api.modrinth.com/v2/project/" + url_encode(slug) + "/version";
    std::string q;
    if (!loader.empty()) q += "loaders=" + url_encode("[\"" + loader + "\"]");
    if (!mcVersion.empty()) {
        if (!q.empty()) q += "&";
        q += "game_versions=" + url_encode("[\"" + mcVersion + "\"]");
    }
    if (!q.empty()) url += "?" + q;

    const json versions = get_json(url, cancel);
    if (!versions.is_array() || versions.empty())
        throw std::runtime_error("Aucune version compatible trouvée sur Modrinth.");

    auto files = versions[0].find("files");
    if (files == versions[0].end() || !files->is_array() || files->empty())
        throw std::runtime_error("La version Modrinth ne contient aucun fichier.");

    // Fichier « primary », sinon le premier (fidele au C#).
    const json* chosen = nullptr;
    for (const auto& f : *files)
        if (f.value("primary", false)) {
            chosen = &f;
            break;
        }
    if (!chosen) chosen = &files->front();

    const std::string fileUrl = chosen->value("url", std::string{});
    std::string fileName = chosen->value("filename", std::string{});
    if (fileUrl.empty()) throw std::runtime_error("Fichier Modrinth sans URL.");
    if (fileName.empty()) fileName = slug + ".jar";

    std::error_code ec;
    fs::create_directories(destDir, ec);
    const fs::path dest = destDir / cf::sanitize(fileName);

    auto r = http::get_response(fileUrl, {}, cancel);
    if (!r || r->status != 200 || r->body.empty())
        throw std::runtime_error("Téléchargement Modrinth impossible (HTTP " +
                                 std::to_string(r ? r->status : 0) + ").");
    std::ofstream out(dest, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Écriture impossible : " + dest.string());
    out.write(r->body.data(), static_cast<std::streamsize>(r->body.size()));
    if (!out) throw std::runtime_error("Écriture incomplète : " + dest.string());
    return dest.string();
}

} // namespace tl::mr

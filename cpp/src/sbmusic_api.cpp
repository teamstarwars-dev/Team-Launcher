#include "sbmusic_api.hpp"

#include "http_win.hpp"
#include "sbmusic.hpp" // access_token

#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>

namespace tl::sbmusic {

namespace {

constexpr const char* kApiBase = "https://sb-music.com/api/v1";

std::string url_encode(const std::string& s) {
    static const char* kHex = "0123456789ABCDEF";
    std::string o;
    o.reserve(s.size());
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~')
            o.push_back(static_cast<char>(c));
        else {
            o.push_back('%');
            o.push_back(kHex[c >> 4]);
            o.push_back(kHex[c & 0x0F]);
        }
    }
    return o;
}

// Lecture SURE d'un champ chaine (jamais d'exception sur type inattendu).
std::string jstr(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string{};
}

int jint(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<int>() : 0;
}

// GET authentifie sur l'API v1. nullopt si non connecte ou echec. Gere un
// unique nouvel essai sur 429 (quota : 40 req / 30 s), en respectant
// Retry-After (plafonne pour ne pas bloquer longtemps).
std::optional<nlohmann::json> api_get(const std::string& path) {
    const std::string hdr = auth_header();
    if (hdr.empty()) return std::nullopt;
    const std::string url = kApiBase + path;

    for (int attempt = 0; attempt < 2; ++attempt) {
        auto r = http::get_response(url, hdr);
        if (!r) return std::nullopt;
        if (r->status == 429 && attempt == 0) {
            int wait = 2;
            try {
                const auto b = nlohmann::json::parse(r->body, nullptr, false);
                if (b.is_object() && b.contains("retryAfter") &&
                    b["retryAfter"].is_number_integer())
                    wait = b["retryAfter"].get<int>();
            } catch (...) {
            }
            if (wait < 1) wait = 1;
            if (wait > 10) wait = 10; // plafond : on ne bloque pas le worker
            std::this_thread::sleep_for(std::chrono::seconds(wait));
            continue;
        }
        if (r->status != 200) return std::nullopt;
        try {
            return nlohmann::json::parse(r->body);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

Track parse_track(const nlohmann::json& t) {
    Track x;
    x.id = jstr(t, "id");
    x.title = jstr(t, "title");
    x.artist = jstr(t, "artist");
    x.album = jstr(t, "album");
    x.thumbnail = jstr(t, "thumbnail");
    x.duration = jint(t, "duration");
    return x;
}

// Trouve un tableau de titres dans une reponse : cle "tracks", sinon "items",
// sinon la racine si c'est deja un tableau.
const nlohmann::json* tracks_array(const nlohmann::json& root) {
    if (root.is_array()) return &root;
    if (root.is_object()) {
        for (const char* key : {"tracks", "items"}) {
            const auto it = root.find(key);
            if (it != root.end() && it->is_array()) return &*it;
        }
    }
    return nullptr;
}

} // namespace

std::string auth_header() {
    const std::string t = access_token();
    return t.empty() ? std::string{} : ("Authorization: Bearer " + t);
}

std::vector<Track> parse_tracks(const std::string& jsonBody) {
    std::vector<Track> out;
    try {
        const auto root = nlohmann::json::parse(jsonBody, nullptr, false);
        if (root.is_discarded()) return out;
        if (const auto* arr = tracks_array(root))
            for (const auto& t : *arr)
                if (t.is_object()) out.push_back(parse_track(t));
    } catch (...) {
    }
    return out;
}

std::vector<Playlist> parse_playlists(const std::string& jsonBody) {
    std::vector<Playlist> out;
    try {
        const auto root = nlohmann::json::parse(jsonBody, nullptr, false);
        if (root.is_discarded()) return out;
        const nlohmann::json* arr = nullptr;
        if (root.is_array()) {
            arr = &root;
        } else if (root.is_object()) {
            for (const char* key : {"playlists", "items"}) {
                const auto it = root.find(key);
                if (it != root.end() && it->is_array()) { arr = &*it; break; }
            }
        }
        if (!arr) return out;
        for (const auto& p : *arr) {
            if (!p.is_object()) continue;
            Playlist x;
            x.id = jstr(p, "id");
            x.name = jstr(p, "name");
            // Nombre de titres : plusieurs noms de champ possibles.
            x.trackCount = jint(p, "trackCount");
            if (x.trackCount == 0) x.trackCount = jint(p, "total");
            out.push_back(std::move(x));
        }
    } catch (...) {
    }
    return out;
}

std::vector<Track> search(const std::string& query, int limit) {
    if (query.empty()) return {};
    const auto j = api_get("/search?q=" + url_encode(query) +
                           "&type=track&limit=" + std::to_string(limit));
    if (!j) return {};
    return parse_tracks(j->dump());
}

std::vector<Track> my_tracks(int limit) {
    const auto j = api_get("/me/tracks?limit=" + std::to_string(limit));
    if (!j) return {};
    return parse_tracks(j->dump());
}

std::vector<Playlist> my_playlists() {
    const auto j = api_get("/me/playlists");
    if (!j) return {};
    return parse_playlists(j->dump());
}

std::vector<Track> playlist_tracks(const std::string& playlistId) {
    if (playlistId.empty()) return {};
    const auto j = api_get("/playlists/" + url_encode(playlistId));
    if (!j) return {};
    return parse_tracks(j->dump());
}

std::string stream_url(const std::string& trackId) {
    if (trackId.empty()) return {};
    return std::string(kApiBase) + "/streams/" + url_encode(trackId);
}

} // namespace tl::sbmusic

#include "sbmusic.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"
#include "oauth_loopback.hpp"
#include "pkce.hpp"
#include "proc.hpp" // open_detached (POSIX) ; vide sous Windows

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif

#include <nlohmann/json.hpp>

#include <cstdint>
#include <ctime>
#include <string>

namespace tl::sbmusic {

namespace {

// client_id PUBLIC de l'application Team Launcher sur SB'Music (OAuth : le
// client_id n'est pas un secret, il est destiné à vivre dans le client).
constexpr const char* kClientId = "b04a1383ace48c844943f71cd39fe9ff";
constexpr int kPort = 7421;
constexpr const char* kRedirect = "http://127.0.0.1:7421/callback";
constexpr const char* kAuthorize = "https://sb-music.com/oauth/authorize";
constexpr const char* kTokenUrl = "https://sb-music.com/oauth/token";
// Scopes demandés : lecture du flux + profil + bibliothèque + playlists.
constexpr const char* kScopes =
    "streaming user-read-profile user-library-read playlist-read-private";

// Encodage pour composant d'URL / corps form-urlencoded (RFC 3986 : seuls les
// caractères « unreserved » restent tels quels).
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

void open_browser(const std::string& url) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (n <= 0) return;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    proc::open_detached(url);
#endif
}

std::int64_t now_unix() { return static_cast<std::int64_t>(std::time(nullptr)); }

// Mémorise les jetons d'une réponse /oauth/token. false si la réponse est
// inexploitable.
bool store_tokens(const nlohmann::json& tok, std::string* err) {
    if (!tok.is_object() || !tok.contains("access_token") ||
        !tok["access_token"].is_string()) {
        if (err) *err = "réponse de jeton invalide";
        return false;
    }
    auto& s = DataStore::settings;
    s.sbMusicAccessToken = tok["access_token"].get<std::string>();
    // Le refresh token n'est pas toujours renvoyé (certains serveurs ne le
    // tournent qu'à l'octroi initial) : on ne l'écrase que s'il est présent.
    if (tok.contains("refresh_token") && tok["refresh_token"].is_string())
        s.sbMusicRefreshToken = tok["refresh_token"].get<std::string>();
    const long long ttl =
        (tok.contains("expires_in") && tok["expires_in"].is_number_integer())
            ? tok["expires_in"].get<long long>()
            : 3600;
    s.sbMusicTokenExpiry = now_unix() + ttl;
    DataStore::save();
    return true;
}

} // namespace

const char* client_id() { return kClientId; }

bool is_logged_in() {
    const auto& s = DataStore::settings;
    return !s.sbMusicAccessToken.empty() || !s.sbMusicRefreshToken.empty();
}

bool login(std::string* err) {
    const std::string verifier = pkce::make_verifier();
    const std::string challenge = pkce::challenge(verifier);
    const std::string state = pkce::make_verifier(); // 43 caractères aléatoires

    const std::string authUrl = std::string(kAuthorize) + "?response_type=code" +
                                "&client_id=" + url_encode(kClientId) +
                                "&redirect_uri=" + url_encode(kRedirect) +
                                "&scope=" + url_encode(kScopes) +
                                "&state=" + url_encode(state) +
                                "&code_challenge=" + url_encode(challenge) +
                                "&code_challenge_method=S256";

    log_line("[SB'Music] ouverture de l'écran d'autorisation OAuth...");
    // Le navigateur n'est ouvert qu'une fois le listener loopback prêt.
    const auto code = oauth::capture_code(kPort, "/callback", state, 120, err,
                                          [&] { open_browser(authUrl); });
    if (!code) {
        log_line("[SB'Music] connexion abandonnée : " +
                 (err && !err->empty() ? *err : std::string("raison inconnue")));
        return false;
    }

    const std::string body = std::string("grant_type=authorization_code") +
                             "&code=" + url_encode(*code) +
                             "&redirect_uri=" + url_encode(kRedirect) +
                             "&client_id=" + url_encode(kClientId) +
                             "&code_verifier=" + url_encode(verifier);
    auto r = http::post_string(kTokenUrl, body, "application/x-www-form-urlencoded");
    if (!r || r->status != 200) {
        if (err)
            *err = "échec de l'échange du jeton" +
                   (r ? (" (HTTP " + std::to_string(r->status) + ")")
                      : std::string(" (réseau)"));
        return false;
    }
    try {
        if (!store_tokens(nlohmann::json::parse(r->body), err)) return false;
    } catch (const std::exception& ex) {
        if (err) *err = std::string("réponse de jeton illisible : ") + ex.what();
        return false;
    }
    log_line("[SB'Music] connecté.");
    return true;
}

void logout() {
    auto& s = DataStore::settings;
    s.sbMusicAccessToken.clear();
    s.sbMusicRefreshToken.clear();
    s.sbMusicTokenExpiry = 0;
    DataStore::save();
}

std::string access_token() {
    auto& s = DataStore::settings;
    // Marge de 30 s : on ne part pas avec un jeton qui expire en vol.
    if (!s.sbMusicAccessToken.empty() && now_unix() < s.sbMusicTokenExpiry - 30)
        return s.sbMusicAccessToken;
    if (s.sbMusicRefreshToken.empty()) return {};

    const std::string body = std::string("grant_type=refresh_token") +
                             "&refresh_token=" + url_encode(s.sbMusicRefreshToken) +
                             "&client_id=" + url_encode(kClientId);
    auto r = http::post_string(kTokenUrl, body, "application/x-www-form-urlencoded");
    if (!r || r->status != 200) {
        log_line("[SB'Music] rafraîchissement du jeton échoué.");
        return {};
    }
    try {
        std::string e;
        if (store_tokens(nlohmann::json::parse(r->body), &e))
            return s.sbMusicAccessToken;
    } catch (...) {
    }
    return {};
}

} // namespace tl::sbmusic

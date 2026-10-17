#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>
#else
// Etape 5 (Linux) : transport libcurl (même API, mêmes délais, mêmes logs).
#include <curl/curl.h>

#include <cstring> // strcmp
#endif

#include "http_win.hpp"

#include "datastore.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace tl::http {

namespace {

#ifdef _WIN32
void dbg(const char* step, DWORD err = ::GetLastError()) {
    if (std::getenv("TL_HTTP_DEBUG"))
        std::fprintf(stderr, "[http] ECHEC %s (GetLastError=%lu)\n", step,
                     static_cast<unsigned long>(err));
}
#else
void dbg(const char* step, CURLcode code = CURLE_OK) {
    if (std::getenv("TL_HTTP_DEBUG"))
        std::fprintf(stderr, "[http] ECHEC %s (%s)\n", step,
                     curl_easy_strerror(code));
}
#endif

#ifdef _WIN32
// Wide -> UTF-8 ( WideCharToMultiByte CP_UTF8 ). Chaine vide si conversion
// impossible (l'appelant journalise alors une chaine vide, jamais un crash).
std::string wstr_to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    const int written = ::WideCharToMultiByte(
        CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr,
        nullptr);
    if (written <= 0) return {};
    return out;
}
#endif // _WIN32 (cote POSIX : curl travaille direct en UTF-8, pas de wide)

// ---------------------------------------------------------------------------
// S2 — allowlist d'hotes
// ---------------------------------------------------------------------------

// Liste de base (grep des URL litterales de cpp/src + CDN references par les
// JSON distants). Correspondance par suffixe avec frontiere de domaine :
//   h == entry  OU  h se termine par "." + entry   (insensible a la casse).
// Une entree « IP » (chiffres/points seulement) n'est acceptee qu'a
// l'identique : sinon « 183.51.255.207.183 » passerait par suffixe.
constexpr const char* kHostAllow[] = {
    // Auth Microsoft / Xbox / Minecraft Services (ms_auth)
    "live.com",           // login.live.com, account.live.com
    "microsoftonline.com", // login.microsoftonline.com
    "microsoft.com",      // account.microsoft.com, go.microsoft.com
    "xbox.com",           // auth.xbox.com
    "xboxlive.com",       // user.auth.xboxlive.com, xsts.auth.xboxlive.com
    "minecraftservices.com", // api.minecraftservices.com
    "aka.ms",             // aides/integration (messages de config)
    // Minecraft / Mojang (manifeste, versions, assets, bibliotheques, textures)
    "mojang.com",     // piston-meta.mojang.com, piston-data, api.mojang.com
    "minecraft.net",  // libraries/resources.download + textures.minecraft.net
    // Plateformes de mods + leur CDN (curseforge/modrinth)
    "modrinth.com",   // api.modrinth.com + cdn.modrinth.com
    "curseforge.com", // api/www/console.curseforge.com
    "forgecdn.net",   // edge/media/mediafilez.forgecdn.net (telechargements)
    // Outils / loaders : meta + maven + installateurs
    "minecraftforge.net", // files.minecraftforge.net + maven.minecraftforge.net
    "neoforged.net",      // maven.neoforged.net
    "fabricmc.net",       // meta.fabricmc.net + maven.fabricmc.net
    "quiltmc.org",        // meta.quiltmc.org + maven.quiltmc.org (Quilt)
    "adoptium.net",       // api.adoptium.net (JRE et JDK)
    "gradle.org",         // services.gradle.org (distributions Gradle)
    // GitHub (API release, raw, Pages du projet)
    "github.com",
    "githubusercontent.com",        // raw. + objects.
    "team-launcher.vercel.app",     // site vitrine (lien « Voir le site »)
    "teamstarwars-dev.github.io",   // ancien site / flux d'actus
    // GitLab : Modrinth autorise cdn.modrinth.com, github.com,
    // raw.githubusercontent.com ET gitlab.com dans les URL de telechargement
    // d'un .mrpack. Sans cette entree, importer un modpack qui heberge un
    // fichier sur GitLab echoue en silence (compte en echec).
    "gitlab.com",
    // Discord (webhook de telemetrie : format actuel + ancien)
    "discord.com",
    "discordapp.com",
    "discord.gg",            // invitations de salon (lien d'entraide)
    // Donnees cartographiques (generateur de ville)
    "overpass-api.de",
    // Skins / vignettes
    "mc-heads.net",
    "namemc.com",    // namemc.com + s.namemc.com
    "archive.org",   // archive.org + web.archive.org (repli NameMC)
    // Infra d'administration
    "51.255.207.183", // IP admin (adminServerUrl par defaut)
    // Boucle locale : tests + services locaux
    "127.0.0.1",
    "localhost",
    "::1",
};

// Hotes supplementaires issus de la config utilisateur (mutex partage).
std::set<std::string>& extra_hosts() {
    static std::set<std::string> hosts;
    return hosts;
}
std::mutex& extra_hosts_mutex() {
    static std::mutex m;
    return m;
}
std::once_flag g_settings_once;

// Normalisation : espaces de bord + point final retires, minuscules,
// crochets IPv6 ([::1]) retires.
std::string normalize_host(std::string h) {
    const auto not_space = [](char c) { return c != ' ' && c != '\t'; };
    h.erase(h.begin(), std::find_if(h.begin(), h.end(), not_space));
    h.erase(std::find_if(h.rbegin(), h.rend(), not_space).base(), h.end());
    while (!h.empty() && h.back() == '.') h.pop_back();
    for (char& c : h)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (h.size() >= 2 && h.front() == '[' && h.back() == ']')
        h = h.substr(1, h.size() - 2);
    return h;
}

bool ipv4_like(const std::string& e) {
    if (e.empty()) return false;
    for (char c : e)
        if (!std::isdigit(static_cast<unsigned char>(c)) && c != '.') return false;
    return true;
}

// h et entry doivent deja etre normalises/minuscules.
bool host_matches(const std::string& h, const std::string& entry) {
    if (entry.empty()) return false;
    if (ipv4_like(entry)) return h == entry; // IP : egalite stricte
    if (h == entry) return true;
    if (h.size() <= entry.size() + 1) return false;
    if (h.compare(h.size() - entry.size(), entry.size(), entry) != 0) return false;
    return h[h.size() - entry.size() - 1] == '.'; // frontiere de domaine
}

// Extrait l'hote d'une URL sans parseur externe : apres « :// », jusqu'au
// prochain « / », « ? » ou « # », puis userinfo@ et :port retires.
std::string host_from_url(const std::string& url) {
    const auto schemeEnd = url.find("://");
    if (schemeEnd == std::string::npos) return {};
    size_t b = schemeEnd + 3;
    size_t e = url.find_first_of("/?#", b);
    if (e == std::string::npos) e = url.size();
    std::string auth = url.substr(b, e - b);
    const auto at = auth.rfind('@');
    if (at != std::string::npos) auth.erase(0, at + 1);
    if (!auth.empty() && auth.front() == '[') {
        const auto close = auth.find(']');
        if (close != std::string::npos) auth.resize(close + 1); // IPv6 garde
    } else {
        const auto colon = auth.rfind(':');
        if (colon != std::string::npos) auth.resize(colon); // :port
    }
    return auth;
}

// Enregistrement lazy des hotes de la config (une seule fois, au 1er controle).
// datastore.cpp n'inclut pas http_win.hpp : pas de dependance circulaire.
void ensure_settings_hosts() {
    std::call_once(g_settings_once, [] {
        const tl::AppSettings& s = tl::DataStore::settings;
        const std::string* urls[] = {&s.adminServerUrl, &s.updateUrl, &s.newsUrl,
                                     &s.discordTelemetryWebhook, &s.vpsUrl,
                                     &s.marketplaceUrl};
        for (const std::string* u : urls)
            if (u && !u->empty()) allow_host(host_from_url(*u));
        if (s.pteroHosts.is_array()) {
            for (const auto& h : s.pteroHosts) {
                if (!h.is_object()) continue;
                auto it = h.find("PanelUrl"); // server_host::host_to_json
                if (it == h.end() || !it->is_string()) continue;
                const std::string u = it->get<std::string>();
                if (!u.empty()) allow_host(host_from_url(u));
            }
        }
    });
}

// ---------------------------------------------------------------------------
// S2 — redaction des secrets dans les journaux (TL_HTTP_DEBUG)
// ---------------------------------------------------------------------------

bool sensitive_query_key(const std::string& keyLower) {
    static const char* kSensitive[] = {"key",       "token",  "access_token",
                                       "refresh_token", "api_key", "apikey",
                                       "code",      "secret", "sig",
                                       "signature", "password", "auth",
                                       "bearer",    "x-api-key"};
    for (const char* s : kSensitive)
        if (keyLower == s) return true;
    return false;
}

std::string lower_ascii(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// /webhooks/<id>/<jeton> -> /webhooks/<id>/*** si >= 2 segments apres
// « webhooks » (Discord). Les autres chemins sont recopies tels quels.
std::string redact_webhook_path(const std::string& p) {
    static const char kName[] = "webhooks";
    constexpr size_t kLen = sizeof(kName) - 1;
    for (size_t i = 0; i + kLen <= p.size(); ++i) {
        if (i != 0 && p[i - 1] != '/') continue; // segment entier
        bool same = true;
        for (size_t k = 0; k < kLen; ++k) {
            if (std::tolower(static_cast<unsigned char>(p[i + k])) !=
                static_cast<unsigned char>(kName[k])) {
                same = false;
                break;
            }
        }
        if (!same) continue;
        if (i + kLen < p.size() && p[i + kLen] != '/') continue;
        const size_t after = i + kLen;
        if (after >= p.size()) return p; // « /webhooks » seul
        size_t idStart = p.find('/', after);
        if (idStart == std::string::npos) return p;
        while (idStart < p.size() && p[idStart] == '/') ++idStart;
        if (idStart >= p.size()) return p;
        size_t tokStart = p.find('/', idStart);
        if (tokStart == std::string::npos) return p; // id seul : rien a masquer
        size_t tokEnd = p.find('/', tokStart + 1);
        if (tokEnd == std::string::npos) tokEnd = p.size();
        return p.substr(0, tokStart + 1) + "***" + p.substr(tokEnd);
    }
    return p;
}

// « ?a=1&b=2 » : les valeurs des cles sensibles deviennent « *** ».
std::string redact_query(const std::string& q) {
    std::string out;
    size_t i = 0;
    if (!q.empty() && q[0] == '?') { // le « ? » n'est pas partie de la cle
        out.push_back('?');
        i = 1;
    }
    while (i < q.size()) {
        size_t sep = q.find_first_of("&;", i);
        if (sep == std::string::npos) sep = q.size();
        std::string pair = q.substr(i, sep - i);
        const size_t eq = pair.find('=');
        if (eq != std::string::npos &&
            sensitive_query_key(lower_ascii(pair.substr(0, eq)))) {
            pair.resize(eq + 1); // on garde la cle d'origine
            pair += "***";
        }
        out += pair;
        if (sep < q.size()) out.push_back(q[sep]);
        i = sep + 1;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Transport WinHTTP (Windows). Version POSIX (libcurl) plus bas.
// ---------------------------------------------------------------------------
#ifdef _WIN32

struct Handle {
    HINTERNET h = nullptr;
    Handle() = default;
    explicit Handle(HINTERNET p) : h(p) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : h(o.h) { o.h = nullptr; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            if (h) WinHttpCloseHandle(h);
            h = o.h;
            o.h = nullptr;
        }
        return *this;
    }
    ~Handle() { if (h) WinHttpCloseHandle(h); }
};

struct Cracked {
    bool secure = false;
    std::wstring host;
    INTERNET_PORT port = 0;
    std::wstring path; // chemin + query
};

std::optional<Cracked> crack(const std::string& url) {
    // n = taille NUL comprise ; le buffer DOIT faire n, sinon la conversion echoue
    const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (n <= 1) return std::nullopt;
    std::wstring wurl(static_cast<size_t>(n), L'\0');
    const int written =
        MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, wurl.data(), n);
    if (written <= 1) {
        dbg("MultiByteToWideChar");
        return std::nullopt;
    }
    wurl.resize(static_cast<size_t>(written - 1));

    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    wchar_t host[512] = {};
    wchar_t path[4096] = {};
    wchar_t extra[4096] = {};
    uc.lpszHostName = host;
    uc.dwHostNameLength = 511;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4095;
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4095;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) {
        dbg("WinHttpCrackUrl");
        return std::nullopt;
    }

    Cracked c;
    c.secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
    c.host = host;
    c.port = uc.nPort;
    c.path = path;
    c.path += extra;
    return c;
}

HINTERNET open_session() {
    HINTERNET s = WinHttpOpen(L"TeamLauncher/6.0",
                              WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                              WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)
        s = WinHttpOpen(L"TeamLauncher/6.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (s)
        WinHttpSetTimeouts(s, 15000, 15000, 30000, 60000);
    return s;
}

// Ouvre la requete + headers de base. nullopt si echec.
// Le handle de connexion DOIT rester ouvert pendant toute la vie de la
// requete (ouvert par l'appelant qui le garde en scope).
// verb : "GET"/"POST"... ; extraHeaders : "Name: value\r\n..." (UTF-8).
Handle make_request(HINTERNET conn, const Cracked& c,
                    const wchar_t* verb = L"GET",
                    const std::string& extraHeaders = {}) {
    DWORD flags = c.secure ? WINHTTP_FLAG_SECURE : 0;
    // lppszAcceptTypes = tableau nul-termine : genere "Accept: */*" (le format
    // "Name: value" de WinHttpAddRequestHeaders n'accepte pas une valeur nue)
    const wchar_t* acceptTypes[] = {L"*/*", nullptr};
    std::wstring wh;
    if (!extraHeaders.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, extraHeaders.c_str(), -1,
                                          nullptr, 0);
        if (n > 1) {
            wh.resize(static_cast<size_t>(n));
            MultiByteToWideChar(CP_UTF8, 0, extraHeaders.c_str(), -1, wh.data(), n);
            wh.resize(static_cast<size_t>(n - 1));
        }
    }
    Handle req{WinHttpOpenRequest(conn, verb, c.path.c_str(), nullptr,
                                  WINHTTP_NO_REFERER, acceptTypes, flags)};
    if (req.h && !wh.empty()) {
        if (!WinHttpAddRequestHeaders(req.h, wh.c_str(),
                                      static_cast<DWORD>(wh.size()),
                                      WINHTTP_ADDREQ_FLAG_ADD)) {
            dbg("WinHttpAddRequestHeaders");
        }
    }
    if (!req.h) {
        dbg("WinHttpOpenRequest");
        if (std::getenv("TL_HTTP_DEBUG"))
            std::fprintf(stderr, "[http] path='%s'\n",
                         redact_url(wstr_to_utf8(c.path)).c_str());
        return {};
    }
    return req;
}

// Attends la reponse, retourne le status HTTP (0 si erreur reseau).
DWORD wait_response(const Handle& req) {
    if (!WinHttpReceiveResponse(req.h, nullptr)) return 0;
    DWORD status = 0;
    DWORD len = sizeof(status);
    if (!WinHttpQueryHeaders(req.h,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &len,
                             WINHTTP_NO_HEADER_INDEX))
        return 0;
    return status;
}

long long content_length(const Handle& req) {
    DWORD len = sizeof(DWORD);
    DWORD sz = 0;
    if (WinHttpQueryHeaders(req.h,
                            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &sz, &len,
                            WINHTTP_NO_HEADER_INDEX))
        return static_cast<long long>(sz);
    return -1;
}

// true si le status HTTP est une redirection a suivre.
bool is_redirect(DWORD status) {
    return status == 301 || status == 302 || status == 303 || status == 307 ||
           status == 308;
}

// En-tete Location de la reponse (UTF-8), nullopt si absent.
std::optional<std::string> query_location(const Handle& req) {
    DWORD len = 0;
    WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &len, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return std::nullopt;
    std::wstring w(len / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(req.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                             w.data(), &len, WINHTTP_NO_HEADER_INDEX))
        return std::nullopt;
    w.resize(len / sizeof(wchar_t));
    while (!w.empty() && (w.back() == L'\0')) w.pop_back();
    return wstr_to_utf8(w);
}

// Desactive le suivi AUTOMATIQUE des redirections : on les valide a la main
// (allowlist + anti-downgrade) a chaque saut. Sans cela WinHTTP suivait toute
// redirection, vers n'importe quel hote, l'allowlist ne valant que sur l'URL
// de depart.
void disable_auto_redirect(const Handle& req) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
}

// Requete generique (GET/POST) : lit le corps quoi que soit le status HTTP
// (OAuth repond 400 avec du JSON utile). nullopt = echec reseau uniquement.
std::optional<Response> do_request(const char* verb, const std::string& url,
                                   const std::string& body,
                                   const std::string& contentType,
                                   const std::string& extraHeaders,
                                   const std::atomic<bool>* cancel) {
    const bool isGet = std::strcmp(verb, "GET") == 0;
    std::string curUrl = url;
    // M4 : hote d'origine. Les en-tetes fournis par l'appelant (Authorization,
    // x-api-key...) ne repartent QUE vers cet hote : sur un saut cross-host, on
    // ne divulgue pas le secret a l'hote de redirection (comme curl).
    const std::string originHost = host_from_url(url);

    // M4 : les redirections sont suivies A LA MAIN (seulement pour GET), en
    // revalidant l'allowlist et en refusant tout downgrade a chaque saut.
    for (int hop = 0; hop < 10; ++hop) {
        auto c = crack(curUrl);
        if (!c) {
            dbg("crack(url)");
            return std::nullopt;
        }
        // S2 : allowlist d'hotes, refuse avant toute connexion sortante.
        if (!host_allowed(wstr_to_utf8(c->host))) {
            if (std::getenv("TL_HTTP_DEBUG"))
                std::fprintf(stderr, "[http] REFUSE %s\n", redact_url(curUrl).c_str());
            return std::nullopt;
        }
        Handle session{open_session()};
        if (!session.h) {
            dbg("WinHttpOpen");
            return std::nullopt;
        }
        Handle conn{WinHttpConnect(session.h, c->host.c_str(), c->port, 0)};
        if (!conn.h) {
            dbg("WinHttpConnect");
            return std::nullopt;
        }
        // En-tetes sensibles seulement vers l'hote d'origine (cf. originHost).
        std::string headers =
            (host_from_url(curUrl) == originHost) ? extraHeaders : std::string{};
        if (!contentType.empty()) {
            if (!headers.empty()) headers += "\r\n";
            headers += "Content-Type: " + contentType;
        }
        // verb ASCII (GET/POST) -> large (WinHttpOpenRequest est wide)
        std::wstring wverb;
        for (const char* p = verb; *p; ++p)
            wverb.push_back(static_cast<wchar_t>(*p));
        Handle req = make_request(conn.h, *c, wverb.c_str(), headers);
        if (!req.h) {
            dbg("make_request");
            return std::nullopt;
        }
        disable_auto_redirect(req);
        const DWORD bodyLen = static_cast<DWORD>(body.size());
        if (!WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                body.empty() ? WINHTTP_NO_REQUEST_DATA
                                             : (LPVOID)body.data(),
                                bodyLen, bodyLen, 0)) {
            dbg("WinHttpSendRequest");
            return std::nullopt;
        }
        const DWORD status = wait_response(req);
        if (status == 0) {
            dbg("WinHttpReceiveResponse/QueryHeaders");
            if (std::getenv("TL_HTTP_DEBUG"))
                std::fprintf(stderr, "[http] status 0 pour %s\n",
                             redact_url(curUrl).c_str());
            return std::nullopt;
        }

        // Redirection : on la valide et on refait la requete sur la cible.
        if (isGet && is_redirect(status)) {
            const auto loc = query_location(req);
            if (!loc) return std::nullopt; // 3xx sans Location : echec
            const auto next = allowed_redirect(curUrl, *loc);
            if (!next) {
                if (std::getenv("TL_HTTP_DEBUG"))
                    std::fprintf(stderr, "[http] REDIRECT REFUSE vers %s\n",
                                 redact_url(*loc).c_str());
                return std::nullopt;
            }
            curUrl = *next;
            continue;
        }

        if (std::getenv("TL_HTTP_DEBUG") && status != 200)
            std::fprintf(stderr, "[http] status HTTP %lu pour %s\n",
                         static_cast<unsigned long>(status),
                         redact_url(curUrl).c_str());

        Response out;
        out.status = static_cast<int>(status);
        for (;;) {
            if (cancel && cancel->load(std::memory_order_relaxed))
                return std::nullopt;
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req.h, &avail)) return std::nullopt;
            if (avail == 0) break;
            const size_t old = out.body.size();
            out.body.resize(old + avail);
            DWORD read = 0;
            if (!WinHttpReadData(req.h, out.body.data() + old, avail, &read))
                return std::nullopt;
            out.body.resize(old + read);
        }
        return out;
    }
    if (std::getenv("TL_HTTP_DEBUG"))
        std::fprintf(stderr, "[http] trop de redirections pour %s\n",
                     redact_url(url).c_str());
    return std::nullopt;
}

#else // POSIX : transport libcurl (même API, mêmes délais 15/60 s, même UA)

// curl_global_init une fois (call_once thread-safe ; requis avant tout easy).
void ensure_curl_init() {
    static std::once_flag f;
    std::call_once(f, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

struct Easy {
    CURL* h = nullptr;
    Easy() : h(curl_easy_init()) {}
    ~Easy() {
        if (h) curl_easy_cleanup(h);
    }
    Easy(const Easy&) = delete;
    Easy& operator=(const Easy&) = delete;
};

struct Slist {
    curl_slist* l = nullptr;
    ~Slist() {
        if (l) curl_slist_free_all(l);
    }
    void append(const std::string& s) { l = curl_slist_append(l, s.c_str()); }
};

size_t write_mem(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* s = static_cast<std::string*>(ud);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

// Fichier : 0 signale l'échec à curl (CURLE_WRITE_ERROR).
size_t write_file(char* ptr, size_t size, size_t nmemb, void* ud) {
    auto* out = static_cast<std::ofstream*>(ud);
    const size_t n = size * nmemb;
    out->write(ptr, static_cast<std::streamsize>(n));
    return out->fail() ? 0 : n;
}

// M4 (reprise) : hote TEL QUE libcurl le parsera (API CURLU), pour que la
// decision d'allowlist porte sur l'hote reellement contacte et non sur un
// parseur maison (host_from_url) qui pourrait diverger — sinon un attaquant
// forgerait une URL validee sur un hote mais connectee a un autre (SSRF par
// confusion de parseur). nullopt si curl rejette l'URL => on refuse.
std::optional<std::string> curl_host(const std::string& url) {
    CURLU* u = curl_url();
    if (!u) return std::nullopt;
    std::optional<std::string> out;
    if (curl_url_set(u, CURLUPART_URL, url.c_str(), 0) == CURLUE_OK) {
        char* host = nullptr;
        if (curl_url_get(u, CURLUPART_HOST, &host, 0) == CURLUE_OK && host)
            out = std::string(host);
        if (host) curl_free(host);
    }
    curl_url_cleanup(u);
    return out;
}

// Vrai si l'URL sera connectee par curl vers un hote autorise. Fail-closed si
// curl ne sait pas parser l'URL.
bool curl_host_allowed(const std::string& url) {
    const auto h = curl_host(url);
    return h && host_allowed(*h);
}

// Annulation + progression. Retour != 0 : curl avorte (CURLE_ABORTED_BY_CALLBACK).
struct XferCtx {
    const std::atomic<bool>* cancel = nullptr;
    ProgressFn* progress = nullptr; // get_to_file uniquement
};

int xfer_cb(void* ud, curl_off_t dltotal, curl_off_t dlnow, curl_off_t, curl_off_t) {
    const auto* ctx = static_cast<const XferCtx*>(ud);
    if (ctx->cancel && ctx->cancel->load(std::memory_order_relaxed)) return 1;
    if (ctx->progress && *ctx->progress)
        (*ctx->progress)(static_cast<long long>(dlnow),
                         dltotal >= 0 ? static_cast<long long>(dltotal) : -1);
    return 0;
}

// « Name: value\r\n... » + Content-Type optionnel -> slist curl.
Slist build_headers(const std::string& extraHeaders, const std::string& contentType) {
    Slist sl;
    std::string headers = extraHeaders;
    if (!contentType.empty()) {
        if (!headers.empty()) headers += "\r\n";
        headers += "Content-Type: " + contentType;
    }
    size_t i = 0;
    while (i < headers.size()) {
        size_t e = headers.find("\r\n", i);
        if (e == std::string::npos) e = headers.size();
        std::string line = headers.substr(i, e - i);
        const size_t b = line.find_first_not_of(" \t");
        const size_t t = line.find_last_not_of(" \t");
        if (b != std::string::npos && line.find(':', b) != std::string::npos)
            sl.append(line.substr(b, t - b + 1));
        i = e + 2;
    }
    return sl;
}

// http(s):// uniquement, insensible à la casse (comme le parseur WinHTTP qui
// rejette les autres schémas).
bool http_scheme(const std::string& url, bool& secure) {
    std::string head = url.substr(0, 8);
    for (char& c : head)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (head.rfind("https://", 0) == 0) {
        secure = true;
        return true;
    }
    if (head.rfind("http://", 0) == 0) {
        secure = false;
        return true;
    }
    return false;
}

// Requete generique (GET/POST) : lit le corps quoi que soit le status HTTP
// (OAuth repond 400 avec du JSON utile). nullopt = echec reseau uniquement.
std::optional<Response> do_request(const char* verb, const std::string& url,
                                   const std::string& body,
                                   const std::string& contentType,
                                   const std::string& extraHeaders,
                                   const std::atomic<bool>* cancel) {
    ensure_curl_init();
    bool secure = false;
    if (!http_scheme(url, secure)) {
        dbg("scheme");
        return std::nullopt;
    }
    // S2 : allowlist d'hotes, refuse avant toute connexion sortante. On valide
    // l'hote TEL QUE curl le parsera (curl_host_allowed), pas via un parseur
    // maison : la validation et la connexion portent ainsi sur le meme hote.
    if (!curl_host_allowed(url)) {
        if (std::getenv("TL_HTTP_DEBUG"))
            std::fprintf(stderr, "[http] REFUSE %s\n", redact_url(url).c_str());
        return std::nullopt;
    }
    Easy easy;
    if (!easy.h) {
        dbg("curl_easy_init");
        return std::nullopt;
    }
    CURL* h = easy.h;
    std::string responseBody;
    Slist headers = build_headers(extraHeaders, contentType);
    // M4 : en-tetes sensibles (Authorization, x-api-key...) seulement vers
    // l'hote d'origine ; sur un saut cross-host, on ne les divulgue pas (curl
    // fait de meme avec FOLLOWLOCATION, mais on suit a la main).
    const std::string originHost = host_from_url(url);
    XferCtx xfer{cancel, nullptr};
    curl_easy_setopt(h, CURLOPT_USERAGENT, "TeamLauncher/6.0");
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.l);
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_mem);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &responseBody);
    // M4 : pas de suivi AUTOMATIQUE — on valide chaque saut a la main
    // (allowlist + anti-downgrade), ce que FOLLOWLOCATION ne permet pas.
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 60000L);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xfer_cb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &xfer);
    const bool isPost = std::strcmp(verb, "POST") == 0;
    const bool isGet = std::strcmp(verb, "GET") == 0;
    if (isPost) {
        curl_easy_setopt(h, CURLOPT_POST, 1L);
        curl_easy_setopt(h, CURLOPT_POSTFIELDS, body.empty() ? "" : body.data());
        curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(body.size()));
    } else if (!isGet) {
        curl_easy_setopt(h, CURLOPT_CUSTOMREQUEST, verb);
    }

    std::string curUrl = url;
    for (int hop = 0; hop < 10; ++hop) {
        // Chaque saut : l'hote (vu par curl) doit etre autorise avant connexion.
        if (!curl_host_allowed(curUrl)) {
            if (std::getenv("TL_HTTP_DEBUG"))
                std::fprintf(stderr, "[http] REFUSE %s\n", redact_url(curUrl).c_str());
            return std::nullopt;
        }
        responseBody.clear();
        curl_easy_setopt(h, CURLOPT_URL, curUrl.c_str());
        // Hors de l'hote d'origine : aucun en-tete personnalise (anti-fuite).
        curl_easy_setopt(h, CURLOPT_HTTPHEADER,
                         host_from_url(curUrl) == originHost ? headers.l : nullptr);
        const CURLcode rc = curl_easy_perform(h);
        if (rc != CURLE_OK) {
            // Annulation : silencieux (comme le return nullopt sur cancel Windows).
            if (rc == CURLE_ABORTED_BY_CALLBACK && cancel &&
                cancel->load(std::memory_order_relaxed))
                return std::nullopt;
            dbg("curl_easy_perform", rc);
            if (std::getenv("TL_HTTP_DEBUG"))
                std::fprintf(stderr, "[http] status 0 pour %s\n",
                             redact_url(curUrl).c_str());
            return std::nullopt;
        }
        long status = 0;
        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);

        // Redirection (GET seulement) : valider puis refaire sur la cible.
        if (isGet && (status == 301 || status == 302 || status == 303 ||
                      status == 307 || status == 308)) {
            char* loc = nullptr;
            curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &loc);
            const auto next =
                loc ? allowed_redirect(curUrl, loc) : std::optional<std::string>{};
            if (!next) {
                if (std::getenv("TL_HTTP_DEBUG") && loc)
                    std::fprintf(stderr, "[http] REDIRECT REFUSE vers %s\n",
                                 redact_url(loc).c_str());
                return std::nullopt;
            }
            curUrl = *next;
            continue;
        }

        if (std::getenv("TL_HTTP_DEBUG") && status != 200)
            std::fprintf(stderr, "[http] status HTTP %ld pour %s\n", status,
                         redact_url(curUrl).c_str());
        Response out;
        out.status = static_cast<int>(status);
        out.body = std::move(responseBody);
        return out;
    }
    if (std::getenv("TL_HTTP_DEBUG"))
        std::fprintf(stderr, "[http] trop de redirections pour %s\n",
                     redact_url(url).c_str());
    return std::nullopt;
}

#endif // _WIN32 (WinHTTP) / POSIX (libcurl)

} // namespace

// --- S2 : API publique d'allowlist / de redaction ---------------------------

bool host_allowed(const std::string& hostUtf8) {
    ensure_settings_hosts(); // hotes de la config, une seule fois
    const std::string h = normalize_host(hostUtf8);
    if (h.empty()) return false;
    for (const char* e : kHostAllow)
        if (host_matches(h, e)) return true;
    std::lock_guard<std::mutex> lk(extra_hosts_mutex());
    for (const auto& e : extra_hosts())
        if (host_matches(h, e)) return true;
    return false;
}

void allow_host(const std::string& hostUtf8) {
    std::string h = hostUtf8;
    if (h.find("://") != std::string::npos) h = host_from_url(h); // URL complete
    h = normalize_host(h);
    if (h.empty()) return;
    // « hôte:port » (hôte nu) : le port n'appartient jamais a l'hote compare.
    const auto colon = h.rfind(':');
    if (colon != std::string::npos && h.find(':') == colon && colon + 1 < h.size()) {
        const auto portBegin =
            h.begin() + static_cast<std::string::difference_type>(colon + 1);
        if (std::all_of(portBegin, h.end(), [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) != 0;
            }))
            h.resize(colon);
    }
    // Un hote ne contient ni separateur d'URL ni espace (saisie abusive).
    if (h.empty() || h.find_first_of("/\\?#@ \t") != std::string::npos) return;
    std::lock_guard<std::mutex> lk(extra_hosts_mutex());
    extra_hosts().insert(h);
}

std::string redact_url(const std::string& url) {
    // 1. « scheme:// » facultatif : sans lui, on ne traite qu'un chemin+query.
    std::string prefix;
    size_t authBegin = 0;
    const auto schemeEnd = url.find("://");
    if (schemeEnd != std::string::npos) {
        prefix = url.substr(0, schemeEnd + 3);
        authBegin = schemeEnd + 3;
    }
    const size_t restBeginRaw = url.find_first_of("/?#", authBegin);
    const size_t restBegin =
        restBeginRaw == std::string::npos ? url.size() : restBeginRaw;

    // 2. userinfo : « scheme://user:pass@host » -> « scheme://***@host ».
    std::string authority = url.substr(authBegin, restBegin - authBegin);
    if (!authority.empty()) {
        const auto at = authority.rfind('@');
        if (at != std::string::npos) authority = "***@" + authority.substr(at + 1);
    }

    // 3. chemin (+ query + fragment).
    const std::string rest = url.substr(restBegin);
    const size_t found = std::min(rest.find('?'), rest.find('#'));
    const size_t pathEnd = found == std::string::npos ? rest.size() : found;
    const std::string path = rest.substr(0, pathEnd);
    const std::string tail = rest.substr(pathEnd);
    const size_t hash = tail.find('#');
    const std::string query = hash == std::string::npos ? tail : tail.substr(0, hash);
    const std::string fragment =
        hash == std::string::npos ? std::string() : tail.substr(hash);

    return prefix + authority + redact_webhook_path(path) + redact_query(query) +
           fragment;
}

namespace {
bool istarts_with(const std::string& s, const char* p) {
    const size_t n = std::strlen(p);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::tolower(static_cast<unsigned char>(s[i])) !=
            static_cast<unsigned char>(p[i]))
            return false;
    return true;
}

// Resout une Location (absolue ou relative) par rapport a l'URL courante.
std::string resolve_url(const std::string& current, const std::string& loc) {
    if (loc.find("://") != std::string::npos) return loc; // deja absolue
    const auto schemeEnd = current.find("://");
    if (schemeEnd == std::string::npos) return loc;
    const size_t authStart = schemeEnd + 3;
    const size_t authEnd = current.find_first_of("/?#", authStart);
    const std::string origin =
        current.substr(0, authEnd == std::string::npos ? current.size() : authEnd);
    if (!loc.empty() && loc.front() == '/') return origin + loc; // racine
    // relatif au repertoire courant
    const size_t lastSlash = current.find_last_of('/');
    if (lastSlash == std::string::npos || lastSlash < authStart)
        return origin + "/" + loc;
    return current.substr(0, lastSlash + 1) + loc;
}
} // namespace

std::optional<std::string> allowed_redirect(const std::string& currentUrl,
                                            const std::string& location) {
    if (location.empty()) return std::nullopt;
    // Une Location contenant un espace, un « \ » ou un caractere de controle
    // est refusee : elle trahit une tentative de confusion de parseur (l'hote
    // vu par notre resolveur differerait de celui ouvert par le transport).
    for (unsigned char ch : location)
        if (ch <= 0x20 || ch == '\\' || ch == 0x7f) return std::nullopt;
    const std::string target = resolve_url(currentUrl, location);
    const bool tgtSecure = istarts_with(target, "https://");
    const bool tgtHttp = istarts_with(target, "http://");
    if (!tgtSecure && !tgtHttp) return std::nullopt; // schema non http(s)
    if (istarts_with(currentUrl, "https://") && !tgtSecure)
        return std::nullopt; // downgrade https -> http refuse
    if (!host_allowed(host_from_url(target))) return std::nullopt;
    return target;
}

std::optional<std::string> get_string(const std::string& url,
                                      const std::atomic<bool>* cancel) {
    auto r = do_request("GET", url, {}, {}, {}, cancel);
    if (!r || r->status != 200) return std::nullopt;
    return r->body;
}

std::optional<Response> get_response(const std::string& url,
                                     const std::string& extraHeaders,
                                     const std::atomic<bool>* cancel) {
    return do_request("GET", url, {}, {}, extraHeaders, cancel);
}

std::optional<Response> post_string(const std::string& url,
                                    const std::string& body,
                                    const std::string& contentType,
                                    const std::string& extraHeaders,
                                    const std::atomic<bool>* cancel) {
    return do_request("POST", url, body, contentType, extraHeaders, cancel);
}

bool get_to_file(const std::string& url, const fs::path& dest,
                 ProgressFn progress, const std::atomic<bool>* cancel, int retries) {
#ifdef _WIN32
    std::error_code ec;
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);

    // M4 : les redirections sont suivies A LA MAIN (allowlist + anti-downgrade
    // revalides a chaque saut). `hops` est un budget separe des re-essais
    // reseau : suivre une redirection ne consomme pas un `attempt`.
    std::string curUrl = url;
    int hops = 0;
    for (int attempt = 1; attempt <= retries;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        auto c = crack(curUrl);
        if (!c) return false;
        // S2 : allowlist d'hotes, refuse avant toute connexion sortante.
        if (!host_allowed(wstr_to_utf8(c->host))) {
            if (std::getenv("TL_HTTP_DEBUG"))
                std::fprintf(stderr, "[http] REFUSE %s\n", redact_url(curUrl).c_str());
            return false;
        }

        bool ok = false;
        bool redirected = false;
        {
            Handle session{open_session()};
            if (session.h) {
                Handle conn{WinHttpConnect(session.h, c->host.c_str(), c->port, 0)};
                if (conn.h) {
                    Handle req = make_request(conn.h, *c);
                    if (req.h) {
                        disable_auto_redirect(req);
                        if (WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                               WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                            const DWORD status = wait_response(req);
                            if (is_redirect(status)) {
                                const auto loc = query_location(req);
                                const auto next =
                                    loc ? allowed_redirect(curUrl, *loc) : std::nullopt;
                                if (!next) {
                                    if (std::getenv("TL_HTTP_DEBUG") && loc)
                                        std::fprintf(stderr,
                                                     "[http] REDIRECT REFUSE vers %s\n",
                                                     redact_url(*loc).c_str());
                                    return false;
                                }
                                curUrl = *next;
                                redirected = true;
                            } else if (status == 200) {
                                const long long total = content_length(req);
                                long long done = 0;
                                fs::remove(dest, ec); // re-essai : tronque proprement
                                std::ofstream out(dest,
                                                  std::ios::binary | std::ios::trunc);
                                if (out) {
                                    ok = true;
                                    for (;;) {
                                        if (cancel &&
                                            cancel->load(std::memory_order_relaxed)) {
                                            ok = false;
                                            break;
                                        }
                                        DWORD avail = 0;
                                        if (!WinHttpQueryDataAvailable(req.h, &avail)) {
                                            ok = false;
                                            break;
                                        }
                                        if (avail == 0) break;
                                        std::string buf(avail, '\0');
                                        DWORD read = 0;
                                        if (!WinHttpReadData(req.h, buf.data(), avail,
                                                             &read) ||
                                            read == 0) {
                                            ok = false;
                                            break;
                                        }
                                        out.write(buf.data(), read);
                                        if (out.fail()) { ok = false; break; }
                                        done += read;
                                        if (progress) progress(done, total);
                                    }
                                    out.close();
                                }
                            }
                        }
                    }
                }
            }
        }
        if (ok) return true;
        if (redirected) {
            if (++hops > 10) return false; // boucle de redirections
            continue;                      // ne consomme pas d'essai reseau
        }
        fs::remove(dest, ec); // pas de fichier partiel (plus sur que le C#)
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
        ++attempt;
    }
    return false;
#else
    ensure_curl_init();
    bool secure = false;
    if (!http_scheme(url, secure)) {
        dbg("scheme");
        return false;
    }
    // S2 : allowlist d'hotes (hote vu par curl), refuse avant connexion.
    if (!curl_host_allowed(url)) {
        if (std::getenv("TL_HTTP_DEBUG"))
            std::fprintf(stderr, "[http] REFUSE %s\n", redact_url(url).c_str());
        return false;
    }

    std::error_code ec;
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);

    // M4 : suivi manuel des redirections (allowlist + anti-downgrade revalides
    // a chaque saut). `hops` est un budget separe des re-essais reseau.
    std::string curUrl = url;
    int hops = 0;
    for (int attempt = 1; attempt <= retries;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        // Chaque saut : l'hote REELLEMENT contacte par curl doit etre autorise.
        if (!curl_host_allowed(curUrl)) {
            if (std::getenv("TL_HTTP_DEBUG"))
                std::fprintf(stderr, "[http] REFUSE %s\n", redact_url(curUrl).c_str());
            return false;
        }
        bool ok = false;
        bool redirected = false;
        {
            Easy easy;
            if (easy.h) {
                CURL* h = easy.h;
                fs::remove(dest, ec); // re-essai/redirection : tronque proprement
                std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                if (out) {
                    XferCtx xfer{cancel, &progress};
                    curl_easy_setopt(h, CURLOPT_URL, curUrl.c_str());
                    curl_easy_setopt(h, CURLOPT_USERAGENT, "TeamLauncher/6.0");
                    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, write_file);
                    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
                    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
                    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
                    curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT_MS, 15000L);
                    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 60000L);
                    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
                    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, xfer_cb);
                    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &xfer);
                    const CURLcode rc = curl_easy_perform(h);
                    out.close();
                    if (rc == CURLE_OK) {
                        long status = 0;
                        curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
                        if (status == 301 || status == 302 || status == 303 ||
                            status == 307 || status == 308) {
                            char* loc = nullptr;
                            curl_easy_getinfo(h, CURLINFO_REDIRECT_URL, &loc);
                            const auto next = loc ? allowed_redirect(curUrl, loc)
                                                  : std::optional<std::string>{};
                            if (!next) {
                                if (std::getenv("TL_HTTP_DEBUG") && loc)
                                    std::fprintf(stderr,
                                                 "[http] REDIRECT REFUSE vers %s\n",
                                                 redact_url(loc).c_str());
                                fs::remove(dest, ec);
                                return false;
                            }
                            curUrl = *next;
                            redirected = true;
                        } else {
                            // 200 exigé comme côté WinHTTP (wait_response == 200).
                            ok = (status == 200) && !out.fail();
                        }
                    } else if (rc != CURLE_ABORTED_BY_CALLBACK) {
                        dbg("curl_easy_perform", rc);
                    }
                }
            }
        }
        if (ok) return true;
        if (redirected) {
            if (++hops > 10) return false; // boucle de redirections
            continue;                      // ne consomme pas d'essai reseau
        }
        fs::remove(dest, ec); // pas de fichier partiel (plus sur que le C#)
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
        ++attempt;
    }
    return false;
#endif
}

} // namespace tl::http

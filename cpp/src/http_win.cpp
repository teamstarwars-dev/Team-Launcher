#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <winhttp.h>

#include "http_win.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace tl::http {

namespace {

void dbg(const char* step, DWORD err = ::GetLastError()) {
    if (std::getenv("TL_HTTP_DEBUG"))
        std::fprintf(stderr, "[http] ECHEC %s (GetLastError=%lu)\n", step,
                     static_cast<unsigned long>(err));
}

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
            std::fprintf(stderr, "[http] path='%ls'\n", c.path.c_str());
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

// Requete generique (GET/POST) : lit le corps quoi que soit le status HTTP
// (OAuth repond 400 avec du JSON utile). nullopt = echec reseau uniquement.
std::optional<Response> do_request(const char* verb, const std::string& url,
                                   const std::string& body,
                                   const std::string& contentType,
                                   const std::string& extraHeaders,
                                   const std::atomic<bool>* cancel) {
    auto c = crack(url);
    if (!c) {
        dbg("crack(url)");
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
    std::string headers = extraHeaders;
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
            std::fprintf(stderr, "[http] status 0 pour %s\n", url.c_str());
        return std::nullopt;
    }
    if (std::getenv("TL_HTTP_DEBUG") && status != 200)
        std::fprintf(stderr, "[http] status HTTP %lu pour %s\n",
                     static_cast<unsigned long>(status), url.c_str());

    Response out;
    out.status = static_cast<int>(status);
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return std::nullopt;
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

} // namespace

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
    auto c = crack(url);
    if (!c) return false;

    std::error_code ec;
    if (dest.has_parent_path()) fs::create_directories(dest.parent_path(), ec);

    for (int attempt = 1; attempt <= retries; ++attempt) {
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        bool ok = false;
        {
            Handle session{open_session()};
            if (session.h) {
                Handle conn{WinHttpConnect(session.h, c->host.c_str(), c->port, 0)};
                if (conn.h) {
                    Handle req = make_request(conn.h, *c);
                    if (req.h &&
                        WinHttpSendRequest(req.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                        wait_response(req) == 200) {
                        const long long total = content_length(req);
                        long long done = 0;
                        fs::remove(dest, ec); // re-essai : tronque proprement
                        std::ofstream out(dest, std::ios::binary | std::ios::trunc);
                        if (out) {
                            ok = true;
                            for (;;) {
                                if (cancel && cancel->load(std::memory_order_relaxed)) {
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
                                if (!WinHttpReadData(req.h, buf.data(), avail, &read) ||
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
        if (ok) return true;
        fs::remove(dest, ec); // pas de fichier partiel (plus sur que le C#)
        if (cancel && cancel->load(std::memory_order_relaxed)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
    }
    return false;
}

} // namespace tl::http

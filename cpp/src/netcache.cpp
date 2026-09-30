#include "netcache.hpp"

#include "datastore.hpp"
#include "util_hash.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace tl::netcache {

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Nom de fichier deduit de l'URL. On passe par MD5 plutot que d'assainir
// l'URL : une URL contient des caracteres interdits sous Windows (« ? »,
// « : », « * »), depasse vite la limite de longueur de chemin, et deux URL
// differentes pourraient s'assainir en le meme nom.
std::string key_of(const std::string& url) {
    const auto d = md5_digest(url);
    if (!d) return {};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char c : *d) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

fs::path path_of(const std::string& url) {
    const std::string k = key_of(url);
    if (k.empty()) return {};
    return dir() / (k + ".bin");
}

} // namespace

fs::path dir() { return DataStore::dir() / "cache" / "http"; }

std::optional<std::string> peek(const std::string& url, long long maxAge) {
    const fs::path p = path_of(url);
    if (p.empty()) return std::nullopt;
    std::lock_guard<std::mutex> lk(lock());
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;

    if (maxAge > 0) {
        const auto t = fs::last_write_time(p, ec);
        if (ec) return std::nullopt;
        // file_time_type n'a pas d'epoque garantie : on passe par l'horloge
        // systeme, seule comparable a `now_unix()` de facon portable.
        const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
        const long long age =
            now_unix() - std::chrono::duration_cast<std::chrono::seconds>(
                             sys.time_since_epoch())
                             .count();
        if (age > maxAge) return std::nullopt;
    }

    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string body((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (body.empty()) return std::nullopt;
    return body;
}

void put(const std::string& url, const std::string& body) {
    // Une reponse vide vient presque toujours d'une erreur : la mettre en
    // cache ferait croire plus tard a un catalogue vide.
    if (body.empty()) return;
    const fs::path p = path_of(url);
    if (p.empty()) return;
    std::lock_guard<std::mutex> lk(lock());
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);

    // Ecriture par fichier temporaire puis remplacement : une coupure ne
    // doit jamais laisser une entree tronquee, qu'on relirait comme du JSON
    // invalide sans savoir pourquoi.
    const fs::path tmp = p.string() + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(body.data(), static_cast<std::streamsize>(body.size()));
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
        if (ec) fs::remove(tmp, ec);
    }
}

void drop(const std::string& url) {
    const fs::path p = path_of(url);
    if (p.empty()) return;
    std::lock_guard<std::mutex> lk(lock());
    std::error_code ec;
    fs::remove(p, ec);
}

void clear() {
    std::lock_guard<std::mutex> lk(lock());
    std::error_code ec;
    fs::remove_all(dir(), ec);
}

long long size_bytes() {
    std::lock_guard<std::mutex> lk(lock());
    std::error_code ec;
    long long n = 0;
    if (!fs::is_directory(dir(), ec)) return 0;
    for (fs::directory_iterator it(dir(), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        n += static_cast<long long>(fs::file_size(it->path(), ec));
    }
    return n;
}

void trim(long long maxBytes) {
    if (maxBytes < 0) return;
    std::lock_guard<std::mutex> lk(lock());
    std::error_code ec;
    if (!fs::is_directory(dir(), ec)) return;

    struct Ent {
        fs::path p;
        fs::file_time_type t;
        long long sz;
    };
    std::vector<Ent> ents;
    long long total = 0;
    for (fs::directory_iterator it(dir(), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const long long sz = static_cast<long long>(fs::file_size(it->path(), ec));
        ents.push_back({it->path(), fs::last_write_time(it->path(), ec), sz});
        total += sz;
    }
    if (total <= maxBytes) return;

    // Les plus anciennes partent d'abord : une entree recemment ecrite a
    // plus de chances de resservir.
    std::sort(ents.begin(), ents.end(),
              [](const Ent& a, const Ent& b) { return a.t < b.t; });
    for (const auto& e : ents) {
        if (total <= maxBytes) break;
        fs::remove(e.p, ec);
        if (!ec) total -= e.sz;
    }
}

Result get(const std::string& url, long long maxAge, const Fetcher& fetch) {
    Result r;
    if (url.empty()) {
        r.error = "URL vide.";
        return r;
    }

    // 1. copie fraiche : aucun reseau.
    if (auto hit = peek(url, maxAge)) {
        r.body = std::move(*hit);
        r.ok = true;
        r.fromCache = true;
        return r;
    }

    // 2. reseau.
    if (fetch) {
        if (auto fresh = fetch(url)) {
            r.body = std::move(*fresh);
            r.ok = true;
            put(url, r.body);
            return r;
        }
    }

    // 3. hors ligne : copie perimee plutot que rien, mais signalee comme
    //    telle pour que l'interface puisse le dire a l'utilisateur.
    if (auto old = peek(url, 0)) {
        r.body = std::move(*old);
        r.ok = true;
        r.fromCache = true;
        r.stale = true;
        return r;
    }

    r.error = "Indisponible : réseau injoignable et rien en cache.";
    return r;
}

void reset_for_tests() { clear(); }

} // namespace tl::netcache

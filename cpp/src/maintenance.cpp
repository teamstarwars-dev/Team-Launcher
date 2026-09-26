#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "maintenance.hpp"

#include "datastore.hpp"
#include "game_installer.hpp"
#include "game_launcher.hpp"
#include "http_win.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

// ---------------------------------------------------------------------------
// HealthService
// ---------------------------------------------------------------------------

namespace tl::health {

std::vector<Check> run_all(const std::atomic<bool>* cancel) {
    std::vector<Check> r;

    // Java
    const bool hasJava = find_java(8).has_value();
    r.push_back({"Java installé", hasJava,
                 hasJava ? "Au moins un Java compatible détecté"
                         : "Aucun Java trouvé - le launcher en téléchargera un "
                           "si besoin"});

    // Espace disque du volume des instances (C# : volume systeme)
    {
        ULARGE_INTEGER freeBytes{};
        const std::wstring root = DataStore::instancesRoot().root_path().wstring();
        if (GetDiskFreeSpaceExW(root.c_str(), &freeBytes, nullptr, nullptr)) {
            const double freeGb =
                static_cast<double>(freeBytes.QuadPart) / 1024.0 / 1024.0 / 1024.0;
            char detail[96];
            std::snprintf(detail, sizeof(detail), "%.1f Go libres sur %s", freeGb,
                          DataStore::instancesRoot().root_name().string().c_str());
            r.push_back({"Espace disque", freeGb > 2.0, detail});
        } else {
            r.push_back({"Espace disque", true, "Non vérifiable"});
        }
    }

    // Serveurs Mojang
    const bool net =
        http::get_string(
            "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", cancel)
            .has_value();
    r.push_back({"Serveurs Mojang joignables", net,
                 net ? "Téléchargements possibles" : "Vérifie ta connexion internet"});

    // Dossiers accessibles (ecriture reelle, comme le C#)
    {
        std::error_code ec;
        fs::create_directories(DataStore::instancesRoot(), ec);
        const fs::path probe = DataStore::instancesRoot() / ".test";
        bool ok = false;
        {
            std::ofstream out(probe, std::ios::binary | std::ios::trunc);
            ok = out.is_open() && (out << "x").good();
        }
        fs::remove(probe, ec);
        r.push_back({"Dossier des instances accessible", ok,
                     ok ? "Lecture/écriture OK" : "Écriture impossible"});
    }

    // Connexion Microsoft (informatif : l'ID client est integre)
    r.push_back({"Connexion Microsoft", true, "ID client intégré au launcher"});

    return r;
}

} // namespace tl::health

// ---------------------------------------------------------------------------
// CleanupService
// ---------------------------------------------------------------------------

namespace tl::cleanup {

namespace {

bool matches(const std::string& name, const char* prefix, const char* ext) {
    const size_t p = std::char_traits<char>::length(prefix);
    const size_t e = std::char_traits<char>::length(ext);
    return name.size() > p + e && name.compare(0, p, prefix) == 0 &&
           name.compare(name.size() - e, e, ext) == 0;
}

} // namespace

Result run() {
    Result res;
    std::error_code ec;

    // Installeurs et archives JRE du dossier runtime
    const fs::path runtime = runtime_root();
    if (fs::is_directory(runtime, ec)) {
        for (const auto& e : fs::directory_iterator(runtime, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            const std::string n = e.path().filename().string();
            if (!(matches(n, "forge-installer-", ".jar") ||
                  matches(n, "neoforge-installer-", ".jar") ||
                  matches(n, "adoptium-jre-", ".zip")))
                continue;
            const auto sz = fs::file_size(e.path(), ec);
            if (ec) continue;
            if (fs::remove(e.path(), ec) && !ec) {
                res.mb += static_cast<double>(sz);
                ++res.files;
            }
        }
    }

    // .zip de plus de 30 jours dans le dossier de donnees
    const auto cutoff = fs::file_time_type::clock::now() - std::chrono::hours(24 * 30);
    if (fs::is_directory(DataStore::dir(), ec)) {
        for (const auto& e : fs::directory_iterator(DataStore::dir(), ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".zip") continue;
            const auto t = fs::last_write_time(e.path(), ec);
            if (ec || t >= cutoff) continue;
            const auto sz = fs::file_size(e.path(), ec);
            if (ec) continue;
            if (fs::remove(e.path(), ec) && !ec) {
                res.mb += static_cast<double>(sz);
                ++res.files;
            }
        }
    }

    res.mb = res.mb / 1024.0 / 1024.0;
    return res;
}

} // namespace tl::cleanup

// ---------------------------------------------------------------------------
// UpdateService (partie portable : verification via l'API GitHub Releases)
// ---------------------------------------------------------------------------

namespace tl::updates {

namespace {

constexpr const char* kLatestApi =
    "https://api.github.com/repos/teamstarwars-dev/Team-Luncher-/releases/latest";

std::vector<int> split_version(const std::string& v) {
    std::vector<int> out;
    std::string cur;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur.push_back(c);
        } else if (c == '.') {
            out.push_back(cur.empty() ? 0 : std::atoi(cur.c_str()));
            cur.clear();
        }
        // tout autre caractere ('v', '-beta'...) est ignore
    }
    if (!cur.empty()) out.push_back(std::atoi(cur.c_str()));
    return out;
}

} // namespace

const char* current_version() { return TL_VERSION_STRING; }

int compare_versions(const std::string& a, const std::string& b) {
    const auto va = split_version(a), vb = split_version(b);
    for (size_t i = 0; i < (std::max)(va.size(), vb.size()); ++i) {
        const int x = i < va.size() ? va[i] : 0;
        const int y = i < vb.size() ? vb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

std::optional<Info> check(std::string* errOut) {
    // L'API GitHub exige un User-Agent ; WinHttpOpen en pose deja un.
    auto r = http::get_response(kLatestApi, "Accept: application/vnd.github+json");
    if (!r || r->status != 200) {
        if (errOut)
            *errOut = "Impossible de vérifier les mises à jour (HTTP " +
                      std::to_string(r ? r->status : 0) + ").";
        return std::nullopt;
    }
    try {
        const json j = json::parse(r->body);
        Info info;
        info.version = j.value("tag_name", std::string{});
        if (!info.version.empty() && (info.version[0] == 'v' || info.version[0] == 'V'))
            info.version.erase(0, 1);
        info.notes = j.value("body", std::string{});
        info.url = j.value("html_url", std::string{});
        if (info.version.empty()) {
            if (errOut) *errOut = "Réponse GitHub inattendue (pas de tag).";
            return std::nullopt;
        }
        if (compare_versions(info.version, current_version()) <= 0)
            return std::nullopt; // deja a jour
        return info;
    } catch (const std::exception& ex) {
        if (errOut) *errOut = std::string("Réponse GitHub illisible : ") + ex.what();
        return std::nullopt;
    }
}

} // namespace tl::updates

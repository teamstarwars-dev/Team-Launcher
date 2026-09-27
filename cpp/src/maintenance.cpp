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
// UpdateService : verification (API GitHub Releases) + staging + application
// differee. Voir maintenance.hpp pour les divergences vs Velopack (C#).
// ---------------------------------------------------------------------------

namespace tl::updates {

namespace {

constexpr const char* kLatestApi =
    "https://api.github.com/repos/teamstarwars-dev/Team-Luncher-/releases/latest";
constexpr const char* kMarkerName = "pending.json";
constexpr const char* kScriptName = "apply-update.bat";

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

std::string trim_copy(const std::string& s) {
    size_t b = 0;
    while (b < s.size() && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n'))
        ++b;
    size_t e = s.size();
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n'))
        --e;
    return s.substr(b, e - b);
}

// DIVERGENCE : le C# passait Settings.UpdateUrl tel quel a Velopack (qui
// savait deriver le flux). Ici une URL personnalisee est utilisee directement
// comme point d'API JSON ; sinon l'API GitHub officielle.
std::string feed_url() {
    const std::string u = trim_copy(DataStore::settings.updateUrl);
    return u.empty() ? kLatestApi : u;
}

std::string to_lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string& s, const char* suffix) {
    const size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

// Ecriture atomique (fichier .tmp + rename) pour ne jamais laisser un
// marqueur tronque si le launcher est tue pendant l'ecriture.
bool write_text_atomic(const fs::path& dest, const std::string& content,
                       std::string* errOut) {
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out.is_open() || !out.write(content.data(),
                                         static_cast<std::streamsize>(content.size()))) {
            if (errOut) *errOut = "Écriture impossible : " + tmp.string();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::remove(dest, ec);
    ec.clear();
    fs::rename(tmp, dest, ec);
    if (ec) {
        if (errOut) *errOut = "Écriture impossible : " + dest.string();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

// ' -> '' pour les litteraux PowerShell entre quotes simples.
std::string ps_quote(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '\'') r += "''";
        else r.push_back(c);
    }
    return r;
}

// " -> "" pour les arguments du .bat.
std::string bat_quote(const std::string& s) {
    std::string r;
    for (char c : s) {
        if (c == '"') r += "\"\"";
        else r.push_back(c);
    }
    return "\"" + r + "\"";
}

std::string wstr_to_utf8_local(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string r(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, r.data(), n, nullptr, nullptr);
    return r;
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

std::string select_asset_url(const json& release, std::string* nameOut,
                             long long* sizeOut) {
    if (nameOut) nameOut->clear();
    if (sizeOut) *sizeOut = -1;
    if (!release.is_object()) return {};
    const auto it = release.find("assets");
    if (it == release.end() || !it->is_array()) return {};
    // Seuls les .zip sont exploitables (deploiement par Expand-Archive dans
    // le script). Parmi eux : « win » + « x64 » d'abord.
    std::string best, bestName;
    long long bestSize = -1;
    int bestScore = -1;
    for (const auto& a : *it) {
        if (!a.is_object()) continue;
        const std::string name = a.value("name", std::string{});
        const std::string url = a.value("browser_download_url", std::string{});
        if (name.empty() || url.empty()) continue;
        const std::string n = to_lower(name);
        if (!ends_with(n, ".zip")) continue;
        int score = 0;
        if (contains(n, "win") || contains(n, "windows")) score += 4;
        if (contains(n, "x64") || contains(n, "x86_64") || contains(n, "win64"))
            score += 4;
        if (contains(n, "linux") || contains(n, "macos") || contains(n, "osx") ||
            contains(n, "arm64") || contains(n, "aarch64"))
            score -= 8;
        if (score > bestScore) {
            bestScore = score;
            best = url;
            bestName = name;
            bestSize = a.value("size", -1LL);
        }
    }
    if (best.empty()) return {};
    if (nameOut) *nameOut = bestName;
    if (sizeOut) *sizeOut = bestSize;
    return best;
}

std::optional<Info> parse_release_json(const std::string& body, std::string* errOut) {
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& ex) {
        if (errOut) *errOut = std::string("Réponse GitHub illisible : ") + ex.what();
        return std::nullopt;
    }
    if (!j.is_object()) {
        if (errOut) *errOut = "Réponse GitHub inattendue (objet JSON attendu).";
        return std::nullopt;
    }
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
        return std::nullopt; // deja a jour (errOut reste vide)
    info.assetUrl = select_asset_url(j, &info.assetName, &info.assetSize);
    // assetUrl vide = release sans zip Windows : l'UI proposera la page web.
    return info;
}

std::optional<Info> check(std::string* errOut) {
    // L'API GitHub exige un User-Agent ; WinHttpOpen en pose deja un.
    auto r = http::get_response(feed_url(), "Accept: application/vnd.github+json");
    if (!r || r->status != 200) {
        if (errOut)
            *errOut = "Impossible de vérifier les mises à jour (HTTP " +
                      std::to_string(r ? r->status : 0) + ").";
        return std::nullopt;
    }
    return parse_release_json(r->body, errOut);
}

std::filesystem::path updates_dir() { return DataStore::dir() / "updates"; }

std::optional<Staged> staged() {
    const fs::path marker = updates_dir() / kMarkerName;
    std::error_code ec;
    if (!fs::is_regular_file(marker, ec)) return std::nullopt;
    std::ifstream in(marker, std::ios::binary);
    if (!in.is_open()) return std::nullopt;
    json j;
    try {
        j = json::parse(std::string((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>()));
    } catch (const std::exception&) {
        return std::nullopt;
    }
    Staged s;
    s.info.version = j.value("version", std::string{});
    s.info.notes = j.value("notes", std::string{});
    s.info.url = j.value("url", std::string{});
    s.info.assetUrl = j.value("assetUrl", std::string{});
    s.info.assetName = j.value("assetName", std::string{});
    s.info.assetSize = j.value("assetSize", -1LL);
    s.file = fs::path(j.value("file", std::string{}));
    if (s.info.version.empty() || s.file.empty()) return std::nullopt;
    if (!fs::is_regular_file(s.file, ec) || fs::file_size(s.file, ec) == 0)
        return std::nullopt;
    return s;
}

bool has_staged() { return staged().has_value(); }

bool clear_staged(std::string* errOut) {
    std::error_code ec;
    if (auto s = staged()) {
        fs::remove(s->file, ec);
        ec.clear();
    }
    fs::remove(updates_dir() / kMarkerName, ec);
    if (ec) {
        if (errOut) *errOut = "Suppression impossible : " + ec.message();
        return false;
    }
    return true;
}

bool download_update(const Info& info, ProgressFn progress,
                     const std::atomic<bool>* cancel, std::string* errOut) {
    if (info.assetUrl.empty()) {
        if (errOut)
            *errOut = "Aucun paquet Windows (.zip) dans cette release : "
                      "passe par la page de la version.";
        return false;
    }
    if (info.version.empty()) {
        if (errOut) *errOut = "Version de mise à jour inconnue.";
        return false;
    }
    std::error_code ec;
    fs::create_directories(updates_dir(), ec);
    const fs::path dest = updates_dir() / ("team-launcher-" + info.version + ".zip");
    const fs::path part = dest.string() + ".part";
    fs::remove(part, ec);
    // Reseau uniquement via tl::http (TLS schannel, sans contournement).
    if (!http::get_to_file(info.assetUrl, part, std::move(progress), cancel)) {
        if (cancel && cancel->load()) {
            if (errOut) *errOut = "Téléchargement annulé.";
        } else if (errOut) {
            *errOut = "Échec du téléchargement du paquet de mise à jour.";
        }
        return false;
    }
    // DIVERGENCE : pas de signature Velopack ; integrity = taille annoncee.
    if (info.assetSize > 0) {
        const uintmax_t got = fs::file_size(part, ec);
        if (ec || got != static_cast<uintmax_t>(info.assetSize)) {
            fs::remove(part, ec);
            if (errOut)
                *errOut = "Paquet incomplet ou corrompu (taille inattendue) : "
                          "téléchargement refusé.";
            return false;
        }
    }
    fs::remove(dest, ec);
    ec.clear();
    fs::rename(part, dest, ec);
    if (ec) {
        if (errOut) *errOut = "Finalisation impossible : " + dest.string();
        fs::remove(part, ec);
        return false;
    }
    const json marker = {{"version", info.version},
                         {"notes", info.notes},
                         {"url", info.url},
                         {"assetUrl", info.assetUrl},
                         {"assetName", info.assetName},
                         {"assetSize", info.assetSize},
                         {"file", dest.string()}};
    if (!write_text_atomic(updates_dir() / kMarkerName, marker.dump(2), errOut)) {
        fs::remove(dest, ec);
        return false;
    }
    return true;
}

bool install_staged_and_restart(std::string* errOut) {
    auto st = staged();
    if (!st) {
        if (errOut) *errOut = "Aucune mise à jour en attente.";
        return false;
    }
    wchar_t exeW[32768];
    const DWORD n = GetModuleFileNameW(nullptr, exeW, 32767);
    if (n == 0 || n >= 32767) {
        if (errOut) *errOut = "Chemin de l'exécutable introuvable.";
        return false;
    }
    const fs::path exe = fs::path(std::wstring(exeW, n));
    const fs::path exeDir = exe.parent_path();
    const DWORD pid = GetCurrentProcessId();
    // UTF-8 explicite : path::string() suivrait la locale ANSI et abimerait
    // les accents dans le .bat et la commande PowerShell.
    const std::string exeUtf8 = wstr_to_utf8_local(exe.wstring().c_str());
    const std::string dirUtf8 = wstr_to_utf8_local(exeDir.wstring().c_str());

    // Le script travaille en relatif (%~dp0 = updates/) : seuls l'exe et sa
    // cible sont absolus (espaces/accents proteges par guillemets).
    const std::string zipName = st->file.filename().string();
    std::string script;
    script += "@echo off\r\n";
    script += "setlocal\r\n";
    script += "cd /d \"%~dp0\"\r\n";
    script += "set \"PID=" + std::to_string(pid) + "\"\r\n";
    script += ":waitloop\r\n";
    script += "tasklist /FI \"PID eq %PID%\" 2>nul | findstr /I /C:\"%PID%\" >nul && (timeout /t 1 /nobreak >nul & goto waitloop)\r\n";
    script += "powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command \"Expand-Archive -Force -Path '" +
              ps_quote(zipName) + "' -DestinationPath '" + ps_quote(dirUtf8) + "'\"\r\n";
    script += "if errorlevel 1 exit /b 1\r\n";
    script += "del " + bat_quote(zipName) + "\r\n";
    script += "del \"" + std::string(kMarkerName) + "\"\r\n";
    script += "start \"\" " + bat_quote(exeUtf8) + "\r\n";
    script += "del \"%~f0\"\r\n";

    const fs::path bat = updates_dir() / kScriptName;
    if (!write_text_atomic(bat, script, errOut)) return false;

    std::wstring cmd = L"cmd.exe /C \"\"" + bat.wstring() + L"\"\"";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cwd = updates_dir().wstring();
    // Detache : survit a la sortie du launcher pour appliquer + relancer.
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, cwd.c_str(),
                        &si, &pi)) {
        if (errOut) *errOut = "Lancement du script d'installation impossible.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace tl::updates

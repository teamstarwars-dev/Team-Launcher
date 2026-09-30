#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <Windows.h>
#else
// Etape 5 (Linux) : statvfs (disque), readlink/fork/exec (maj), getpid.
#include <cerrno>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "maintenance.hpp"

#include "datastore.hpp"
#include "game_installer.hpp"
#include "game_launcher.hpp"
#include "http_win.hpp"
#include "startup.hpp" // exe_path_utf8 (deploiement : installe ou portable)
#include "util_str.hpp" // wide_to_utf8 (wstr_to_utf8_local)
#include "util_zip.hpp" // zip_extract_all (maj Linux en process)

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
#ifdef _WIN32
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
#else
        struct statvfs sv{};
        if (::statvfs(DataStore::instancesRoot().string().c_str(), &sv) == 0) {
            const double freeGb = static_cast<double>(sv.f_bavail) * sv.f_frsize /
                                  1024.0 / 1024.0 / 1024.0;
            char detail[96];
            std::snprintf(detail, sizeof(detail), "%.1f Go libres", freeGb);
            r.push_back({"Espace disque", freeGb > 2.0, detail});
        } else {
            r.push_back({"Espace disque", true, "Non vérifiable"});
        }
#endif
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
// Canal beta : /releases/latest ne renvoie JAMAIS de preversion, quoi
// qu'on lui demande. Il faut la liste complete, et y choisir soi-meme.
constexpr const char* kListApi =
    "https://api.github.com/repos/teamstarwars-dev/Team-Luncher-/releases?per_page=20";
constexpr const char* kMarkerName = "pending.json";
#ifdef _WIN32
constexpr const char* kScriptName = "apply-update.bat";
#endif

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
bool beta_channel() {
    return trim_copy(DataStore::settings.updateChannel) == "beta";
}

std::string feed_url() {
    // Une URL posee a la main l'emporte sur le canal : elle sert justement
    // a pointer ailleurs que sur le depot officiel.
    const std::string u = trim_copy(DataStore::settings.updateUrl);
    if (!u.empty()) return u;
    return beta_channel() ? kListApi : kLatestApi;
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
#ifdef _WIN32
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
#endif // _WIN32 (ps_quote/bat_quote : script .bat uniquement)

#ifdef _WIN32
std::string wstr_to_utf8_local(const wchar_t* w) { return tl::wide_to_utf8(w); }
#endif // (install_staged POSIX travaille direct en UTF-8)

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
    // Seuls les .zip sont exploitables. Preference OS courant + x64.
    // (Deploiement : Expand-Archive sous Windows, extraction interne sous Linux.)
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
#ifdef _WIN32
        if (contains(n, "win") || contains(n, "windows")) score += 4;
        if (contains(n, "x64") || contains(n, "x86_64") || contains(n, "win64"))
            score += 4;
        if (contains(n, "linux") || contains(n, "macos") || contains(n, "osx") ||
            contains(n, "arm64") || contains(n, "aarch64"))
            score -= 8;
#else
        if (contains(n, "linux")) score += 4;
        if (contains(n, "x64") || contains(n, "x86_64")) score += 4;
        if (contains(n, "win") || contains(n, "windows") || contains(n, "win64") ||
            contains(n, "macos") || contains(n, "osx") || contains(n, "arm64") ||
            contains(n, "aarch64"))
            score -= 8;
#endif
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

namespace {

// Une entree de release -> Info, sans comparaison de version : le tri du
// canal beta a besoin de lire la version AVANT de decider.
std::optional<Info> release_of(const json& j) {
    if (!j.is_object()) return std::nullopt;
    Info info;
    info.version = j.value("tag_name", std::string{});
    if (!info.version.empty() && (info.version[0] == 'v' || info.version[0] == 'V'))
        info.version.erase(0, 1);
    if (info.version.empty()) return std::nullopt;
    info.notes = j.value("body", std::string{});
    info.url = j.value("html_url", std::string{});
    info.assetUrl = select_asset_url(j, &info.assetName, &info.assetSize);
    return info;
}

} // namespace

std::optional<Info> parse_release_json(const std::string& body, std::string* errOut) {
    json j;
    try {
        j = json::parse(body);
    } catch (const std::exception& ex) {
        if (errOut) *errOut = std::string("Réponse GitHub illisible : ") + ex.what();
        return std::nullopt;
    }
    // Canal beta : le flux est la LISTE des releases. On garde la plus
    // haute version utilisable — les brouillons jamais (ils n'ont pas de
    // binaire publie), les preversions seulement en beta. On ne se fie pas
    // a l'ordre renvoye par GitHub, qui trie par date de publication : une
    // correction publiee apres coup sur une ancienne branche passerait
    // devant la version la plus recente.
    if (j.is_array()) {
        const bool beta = beta_channel();
        std::optional<Info> best;
        for (const auto& e : j) {
            if (!e.is_object()) continue;
            if (e.value("draft", false)) continue;
            if (!beta && e.value("prerelease", false)) continue;
            auto info = release_of(e);
            if (!info) continue;
            if (compare_versions(info->version, current_version()) <= 0) continue;
            if (!best || compare_versions(info->version, best->version) > 0)
                best = std::move(info);
        }
        return best; // nullopt = deja a jour (errOut reste vide)
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

namespace {

// Dossier de l'executable courant (vide si introuvable).
fs::path exe_dir() {
    const std::string p = startup::exe_path_utf8();
    if (p.empty()) return {};
    return fs::path(p).parent_path();
}

} // namespace

Deploy deployment() {
    const fs::path dir = exe_dir();
    if (dir.empty()) return Deploy::Portable;
    std::error_code ec;
#ifdef _WIN32
    // Inno Setup depose toujours son desinstalleur a cote du programme :
    // sa presence est le seul marqueur fiable, et il ne peut pas se
    // retrouver la par accident dans un dossier portable.
    if (fs::is_regular_file(dir / "unins000.exe", ec)) return Deploy::Installed;
#else
    // Paquet .deb ou install.sh : le binaire vit sous /usr ou /opt, ou
    // l'utilisateur n'ecrit pas. Un dossier decompresse dans son profil
    // reste portable.
    const std::string s = dir.string();
    if (s.rfind("/usr", 0) == 0 || s.rfind("/opt", 0) == 0)
        return Deploy::Installed;
#endif
    return Deploy::Portable;
}

bool install_dir_writable() {
    const fs::path dir = exe_dir();
    if (dir.empty()) return false;
    // Les droits ne se deduisent pas des permissions affichees (ACL,
    // virtualisation, montage en lecture seule) : on essaie d'ecrire.
    std::error_code ec;
    const fs::path probe = dir / ".tl-write-test";
    {
        std::ofstream out(probe, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << 'x';
        if (!out) return false;
    }
    fs::remove(probe, ec);
    return true;
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
#ifdef _WIN32
            *errOut = "Aucun paquet Windows (.zip) dans cette release : "
                      "passe par la page de la version.";
#else
            *errOut = "Aucun paquet Linux (.zip) dans cette release : "
                      "passe par la page de la version.";
#endif
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
#ifdef _WIN32
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
#else
    // Sous Linux l'exécutable en cours accepte rename() mais pas
    // open(O_TRUNC) (ETXTBSY — idem pour les .so mappés) : on déploie via un
    // dossier de staging + renames atomiques, puis relance détachée.
    // L'appelant quitte ensuite comme côté Windows.
    char exeBuf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", exeBuf, sizeof(exeBuf) - 1);
    if (n <= 0) {
        if (errOut) *errOut = "Chemin de l'exécutable introuvable.";
        return false;
    }
    exeBuf[n] = '\0';
    const fs::path exe(exeBuf);
    const fs::path exeDir = exe.parent_path();
    std::error_code ec2;
    // Ménage d'un staging orphelin (crash pendant une maj précédente).
    for (fs::directory_iterator it(exeDir, ec2), end; it != end;
         it.increment(ec2)) {
        if (ec2) break;
        const std::string nm = it->path().filename().string();
        if (it->is_directory(ec2) && nm.rfind(".tl-update-", 0) == 0)
            fs::remove_all(it->path(), ec2);
    }
    ec2.clear();
    // 1. extraction vers staging (fichiers neufs : jamais d'ETXTBSY).
    const fs::path stage = exeDir / (".tl-update-" + std::to_string(::getpid()));
    fs::remove_all(stage, ec2);
    ec2.clear();
    const int nWrote = zip_extract_all(st->file, stage);
    if (nWrote <= 0) {
        fs::remove_all(stage, ec2);
        if (errOut) *errOut = "Extraction du paquet impossible.";
        return false;
    }
    // 2. bascule par rename (atomique par fichier, marche sur l'exe et les
    // .so mappés — seul open(O_TRUNC) est interdit dessus, pas rename).
    bool moved = true;
    for (fs::recursive_directory_iterator it(stage, ec2), end;
         it != end && moved; it.increment(ec2)) {
        if (ec2) {
            moved = false;
            break;
        }
        if (!it->is_regular_file(ec2)) continue;
        std::error_code ec3;
        const fs::path rel = fs::relative(it->path(), stage, ec3);
        if (ec3) {
            moved = false;
            break;
        }
        const fs::path dst = exeDir / rel;
        fs::create_directories(dst.parent_path(), ec3);
        if (ec3) {
            moved = false;
            break;
        }
        fs::rename(it->path(), dst, ec3);
        if (ec3) moved = false;
    }
    fs::remove_all(stage, ec2);
    if (!moved) {
        if (errOut) *errOut = "Déploiement du paquet impossible.";
        return false;
    }
    // 3. le zip ne garde pas les bits +x : restaure l'exécutable.
    fs::permissions(exe,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec2);
    // 4. ménage : zip + marqueur.
    fs::remove(st->file, ec2);
    fs::remove(updates_dir() / kMarkerName, ec2);
    // 5. relance détachée (double fork : pas de zombie, survit à notre
    // sortie). Entre fork et exec : que des appels async-signal-safe.
    const pid_t f1 = ::fork();
    if (f1 < 0) {
        if (errOut) *errOut = "Relance impossible.";
        return false;
    }
    if (f1 == 0) {
        const pid_t f2 = ::fork();
        if (f2 != 0) _exit(f2 < 0 ? 1 : 0);
        ::setsid();
        const int devnull = ::open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            ::dup2(devnull, STDIN_FILENO);
            ::dup2(devnull, STDOUT_FILENO);
            ::dup2(devnull, STDERR_FILENO);
            if (devnull > STDERR_FILENO) ::close(devnull);
        }
        ::execl(exe.c_str(), exe.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    int st1 = 0;
    while (::waitpid(f1, &st1, 0) < 0 && errno == EINTR) {
    }
    if (!WIFEXITED(st1) || WEXITSTATUS(st1) != 0) {
        if (errOut) *errOut = "Relance impossible.";
        return false;
    }
    return true;
#endif
}

} // namespace tl::updates

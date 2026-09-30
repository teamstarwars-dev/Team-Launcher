#include "support.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp"
#include "maintenance.hpp"
#include "util_zip.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#else
#include <sys/utsname.h>
#endif

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

namespace fs = std::filesystem;

namespace tl::support {

namespace {

constexpr const char* kRepo = "https://github.com/teamstarwars-dev/Team-Luncher-";

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string platform_line() {
#ifdef _WIN32
    // GetVersionEx ment depuis Windows 8.1 sans manifeste ; RtlGetVersion,
    // elle, dit la verite. Elle n'est pas dans un .lib d'import : on la
    // resout a l'execution, et on se rabat sur « Windows » si elle manque.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
        if (fn) {
            RTL_OSVERSIONINFOW vi{};
            vi.dwOSVersionInfoSize = sizeof(vi);
            if (fn(&vi) == 0) {
                char buf[128];
                std::snprintf(buf, sizeof(buf), "Windows %lu.%lu build %lu",
                              static_cast<unsigned long>(vi.dwMajorVersion),
                              static_cast<unsigned long>(vi.dwMinorVersion),
                              static_cast<unsigned long>(vi.dwBuildNumber));
                return buf;
            }
        }
    }
    return "Windows";
#else
    utsname u{};
    if (uname(&u) == 0)
        return std::string(u.sysname) + " " + u.release + " " + u.machine;
    return "Linux";
#endif
}

std::string now_stamp() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return buf;
}

// Copie au plus `maxBytes` octets de FIN de fichier : un launcher.log de
// 40 Mo ne rentre pas dans un ticket, et ce sont les dernieres lignes qui
// racontent la panne.
bool copy_tail(const fs::path& src, const fs::path& dst, long long maxBytes) {
    std::error_code ec;
    if (!fs::is_regular_file(src, ec)) return false;
    const auto size = static_cast<long long>(fs::file_size(src, ec));
    if (ec) return false;
    std::ifstream in(src, std::ios::binary);
    if (!in) return false;
    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (size > maxBytes) {
        in.seekg(size - maxBytes, std::ios::beg);
        out << "[... " << (size - maxBytes) << " octets plus anciens omis ...]\n";
    }
    out << in.rdbuf();
    return static_cast<bool>(out);
}

} // namespace

std::string help_center_url() { return std::string(kRepo) + "/wiki"; }
std::string ticket_url() { return std::string(kRepo) + "/issues/new"; }
std::string suggestion_url() {
    return std::string(kRepo) + "/issues/new?labels=suggestion";
}

bool is_sensitive_key(const std::string& key) {
    const std::string k = lower(key);
    static const char* const kNeedles[] = {
        "apikey",   "api_key", "key",      "token",     "secret",
        "password", "webhook", "clientid", "client_id", "serverurl",
    };
    for (const char* n : kNeedles)
        if (k.find(n) != std::string::npos) return true;
    return false;
}

nlohmann::json redacted_config(const nlohmann::json& raw) {
    if (raw.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            if (is_sensitive_key(it.key())) {
                // On garde la cle : savoir qu'une valeur EST renseignee est
                // souvent le diagnostic ; savoir laquelle ne l'est jamais.
                const bool empty = it.value().is_string() &&
                                   it.value().get<std::string>().empty();
                out[it.key()] = empty ? "(vide)" : "(masqué)";
            } else {
                out[it.key()] = redacted_config(it.value());
            }
        }
        return out;
    }
    if (raw.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& e : raw) out.push_back(redacted_config(e));
        return out;
    }
    return raw;
}

std::string system_report() {
    const auto& s = DataStore::settings;
    std::ostringstream o;
    o << "Team Launcher - rapport de diagnostic\n";
    o << "Date            : " << now_stamp() << "\n";
    o << "Version         : " << TL_VERSION_STRING << "\n";
    o << "Canal           : " << s.updateChannel << "\n";
    o << "Deploiement     : "
      << (updates::deployment() == updates::Deploy::Installed ? "installe"
                                                              : "portable")
      << "\n";
    o << "Plateforme      : " << platform_line() << "\n";
    const long long ram = total_ram_mb();
    o << "RAM machine     : "
      << (ram > 0 ? std::to_string((ram + 512) / 1024) : std::string("?"))
      << " Go\n";
    o << "RAM allouee     : " << s.maxRamGb << " Go\n";
    o << "Java            : "
      << (s.javaPath.empty() ? "(detection automatique)" : s.javaPath) << "\n";
    o << "Dossier donnees : " << DataStore::dir().string() << "\n";
    o << "Instances       : " << DataStore::instancesRoot().string() << "\n";
    o << "Nb d'instances  : " << s.instances.size() << "\n";
    o << "Langue          : " << s.language << "\n";
    o << "Theme           : " << s.theme << "\n";
    o << "Journalisation  : " << s.logLevel << "\n";
    o << "Telech. para.   : " << s.maxDownloads << "\n";
    o << "Fermeture       : " << s.closeBehavior << "\n";
    o << "Demarrage syst. : " << (s.launchAtSystemStart ? "oui" : "non") << "\n";
    o << "\nDiagnostic :\n";
    for (const auto& c : health::run_all())
        o << "  [" << (c.ok ? "OK " : "KO ") << "] " << c.name << " - "
          << c.detail << "\n";
    return o.str();
}

bool export_logs(const fs::path& zipPath, std::string* errOut) {
    auto fail = [&](const std::string& m) {
        if (errOut) *errOut = m;
        return false;
    };
    std::error_code ec;
    // zip_create_from_dir compresse un DOSSIER : on rassemble d'abord les
    // pieces dans un dossier de travail, qu'on efface ensuite.
    const fs::path stage = DataStore::dir() / "support-tmp";
    fs::remove_all(stage, ec);
    fs::create_directories(stage, ec);
    if (ec) return fail("Dossier temporaire impossible à créer.");

    {
        std::ofstream out(stage / "rapport.txt", std::ios::binary);
        if (!out) {
            fs::remove_all(stage, ec);
            return fail("Écriture du rapport impossible.");
        }
        out << system_report();
    }

    copy_tail(DataStore::dir() / "launcher.log", stage / "launcher.log",
              4LL * 1024 * 1024);

    // config.json expurge. Illisible ou absent : on le dit dans l'archive
    // plutot que d'echouer — le journal reste utile a lui seul.
    {
        std::ofstream out(stage / "config-expurge.json", std::ios::binary);
        std::ifstream in(DataStore::configPath(), std::ios::binary);
        if (out) {
            if (in) {
                const std::string body((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
                try {
                    out << redacted_config(nlohmann::json::parse(body)).dump(2);
                } catch (const std::exception& ex) {
                    out << "{\"erreur\":\"config.json illisible\",\"detail\":"
                        << nlohmann::json(std::string(ex.what())).dump() << "}";
                }
            } else {
                out << "{\"erreur\":\"config.json absent\"}";
            }
        }
    }

    if (zipPath.has_parent_path()) fs::create_directories(zipPath.parent_path(), ec);
    const bool ok = zip_create_from_dir(stage, zipPath);
    fs::remove_all(stage, ec);
    if (!ok) return fail("Création de l'archive impossible.");
    return true;
}

} // namespace tl::support

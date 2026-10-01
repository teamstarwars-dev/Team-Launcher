#include "game_launcher.hpp"

#include "datastore.hpp"
#include "game_installer.hpp"
#include "http_win.hpp"
#include "proc.hpp" // posix_spawn (POSIX) ; vide sous Windows
#include "util_hash.hpp"
#include "util_str.hpp"
#include "util_zip.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
// Etape 5 (Linux) : sysinfo (RAM), strcasecmp/access/read (Java, process).
#include <strings.h>
#include <sys/sysinfo.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;

namespace tl {

// ---------------------------------------------------------------------------
// Session offline (Md5 OfflinePlayer + endianness Guid .NET)
// ---------------------------------------------------------------------------

McSession offline_session(const std::string& name) {
    McSession s;
    s.name = name;
    s.accessToken = "0";
    const auto digest = md5_digest("OfflinePlayer:" + name);
    if (digest) {
        // Guid .NET : octets 0-3, 4-5, 6-7 inverses (little-endian), 8-15 bruts.
        const unsigned char* h = digest->data();
        char buf[33];
        std::snprintf(buf, sizeof(buf),
                      "%02x%02x%02x%02x%02x%02x%02x%02x"
                      "%02x%02x%02x%02x%02x%02x%02x%02x",
                      h[3], h[2], h[1], h[0],
                      h[5], h[4], h[7], h[6],
                      h[8], h[9], h[10], h[11], h[12], h[13], h[14], h[15]);
        s.uuid = buf;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Journal
// ---------------------------------------------------------------------------

void log_line(const std::string& text) {
    try {
        const fs::path p = DataStore::dir() / "launcher.log";
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
        const auto t = std::time(nullptr);
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char stamp[16];
        std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
        std::ofstream out(p, std::ios::app);
        out << "[" << stamp << "] " << text << "\n\n";
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// RAM
// ---------------------------------------------------------------------------

namespace {
long long mem_status(bool total) {
#ifdef _WIN32
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m))
        return static_cast<long long>((total ? m.ullTotalPhys : m.ullAvailPhys) /
                                      (1024ull * 1024ull));
    return -1;
#else
    struct sysinfo si{};
    if (::sysinfo(&si) == 0) {
        const unsigned long long bytes =
            static_cast<unsigned long long>(total ? si.totalram : si.freeram) *
            si.mem_unit;
        return static_cast<long long>(bytes / (1024ull * 1024ull));
    }
    return -1;
#endif
}
} // namespace

long long available_ram_mb() { return mem_status(false); }
long long total_ram_mb() { return mem_status(true); }

int ideal_ram_gb(long long totalMb) {
    // Moitie de la RAM physique : laisser autant a Windows/Linux, au
    // navigateur et au reste. Au-dela de 8 Go la JVM de Minecraft ne gagne
    // plus rien et le ramasse-miettes fait des pauses plus longues ; en
    // dessous de 2 Go le jeu ne demarre pas correctement.
    if (totalMb <= 0) return 4; // machine indeterminee : valeur sure
    const long long half = totalMb / 2 / 1024;
    if (half < 2) return 2;
    if (half > 8) return 8;
    return static_cast<int>(half);
}

int ideal_ram_gb() { return ideal_ram_gb(total_ram_mb()); }

// ---------------------------------------------------------------------------
// Java
// ---------------------------------------------------------------------------

namespace {
std::mutex& java_cache_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, int>& java_cache() {
    static std::map<std::string, int> c;
    return c;
}

std::vector<std::string> find_javaw_in(const fs::path& dir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (auto it = fs::recursive_directory_iterator(
             dir, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
#ifdef _WIN32
        if (it->is_regular_file(ec) && _stricmp(it->path().filename().string().c_str(),
                                                "javaw.exe") == 0)
            out.push_back(it->path().string());
#else
        // Linux : les JRE fournissent bin/java (pas de javaw), executable requis.
        if (it->is_regular_file(ec) &&
            ::strcasecmp(it->path().filename().string().c_str(), "java") == 0 &&
            ::access(it->path().string().c_str(), X_OK) == 0)
            out.push_back(it->path().string());
#endif
    }
    return out;
}
} // namespace

int detect_java_major(const std::string& javawPath) {
    {
        std::lock_guard<std::mutex> g(java_cache_mutex());
        auto it = java_cache().find(javawPath);
        if (it != java_cache().end()) return it->second;
    }

    int result = 0;
    try {
        const fs::path dir = fs::path(javawPath).parent_path();
#ifdef _WIN32
        const fs::path javaExe = dir / "java.exe";
#else
        const fs::path javaExe = dir / "java";
#endif
        const fs::path exe = fs::exists(javaExe) ? javaExe : fs::path(javawPath);

#ifdef _WIN32
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE outR = nullptr, outW = nullptr, errW = nullptr;
        if (CreatePipe(&outR, &outW, &sa, 0)) {
            SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
            HANDLE errR = nullptr;
            if (CreatePipe(&errR, &errW, &sa, 0)) {
                SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

                STARTUPINFOW si{};
                si.cb = sizeof(si);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdOutput = outW;
                si.hStdError = errW;
                si.hStdInput = nullptr;
                PROCESS_INFORMATION pi{};
                std::wstring cmd = L"\"" + utf8_to_wide(exe.string()) + L"\" -version";
                std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
                cmdBuf.push_back(L'\0');
                if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(outW); CloseHandle(errW); outW = errW = nullptr;
                    if (WaitForSingleObject(pi.hProcess, 5000) == WAIT_TIMEOUT)
                        TerminateProcess(pi.hProcess, 1);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);

                    std::string output;
                    char buf[4096];
                    DWORD read = 0;
                    while (ReadFile(outR, buf, sizeof(buf), &read, nullptr) && read > 0)
                        output.append(buf, read);
                    while (ReadFile(errR, buf, sizeof(buf), &read, nullptr) && read > 0)
                        output.append(buf, read);

                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                    CloseHandle(outR);
                    CloseHandle(errR);

                    std::smatch m;
                    static const std::regex re("version \"(\\d+)(?:\\.(\\d+))?");
                    if (std::regex_search(output, m, re)) {
                        const int first = std::stoi(m[1].str());
                        result = first == 1 ? std::stoi(m[2].str()) : first;
                    }
                } else {
                    CloseHandle(outR); CloseHandle(errR);
                    CloseHandle(outW); CloseHandle(errW);
                }
            } else {
                CloseHandle(outR); CloseHandle(outW);
            }
        }
#else
        // `java -version` écrit sur stderr : capture fusionnée, 5 s comme Windows.
        const auto r = proc::run_capture(exe.string(), {exe.string(), "-version"},
                                         dir.string(), 5000);
        if (r.code != -1) {
            std::smatch m;
            static const std::regex re("version \"(\\d+)(?:\\.(\\d+))?");
            if (std::regex_search(r.output, m, re)) {
                const int first = std::stoi(m[1].str());
                result = first == 1 ? std::stoi(m[2].str()) : first;
            }
        }
#endif
    } catch (...) {
        result = 0;
    }

    {
        std::lock_guard<std::mutex> g(java_cache_mutex());
        java_cache()[javawPath] = result;
    }
    log_line("Java détecté : " + javawPath + " → version " + std::to_string(result));
    return result;
}

// Cache du balayage : il parcourt plusieurs arborescences et lance un
// `java -version` par candidat, soit plusieurs centaines de millisecondes
// a chaque lancement de partie. La cle inclut le reglage JavaPath : s'il
// change, l'ancien resultat n'a plus de raison d'etre.
//
// Pas d'expiration : un Java installe ou desinstalle pendant la session
// est assez rare pour ne pas valoir de re-balayer a chaque fois. Le bouton
// « Diagnostic du système » et un redemarrage repartent de zero.
namespace {
std::mutex& scan_cache_mutex() {
    static std::mutex m;
    return m;
}
std::map<std::string, std::optional<std::string>>& scan_cache() {
    static std::map<std::string, std::optional<std::string>> c;
    return c;
}
} // namespace

void forget_java_scan() {
    std::lock_guard<std::mutex> g(scan_cache_mutex());
    scan_cache().clear();
}

std::optional<std::string> find_java(int requiredMajor) {
    if (const fs::path configured = fs::path(DataStore::settings.javaPath);
        !configured.empty() && fs::exists(configured))
        return configured.string();

    const std::string key = std::to_string(requiredMajor) + "|" +
                            DataStore::settings.javaPath;
    {
        std::lock_guard<std::mutex> g(scan_cache_mutex());
        auto it = scan_cache().find(key);
        if (it != scan_cache().end()) return it->second;
    }

    log_line("Recherche des Java installés...");
    std::vector<std::string> candidates;

#ifdef _WIN32
    const char* roots[] = {
        "C:\\Program Files\\Java",
        "C:\\Program Files\\Eclipse Adoptium",
        "C:\\Program Files\\Amazon Corretto",
        "C:\\Program Files\\Zulu",
        "C:\\Program Files (x86)\\Java",
    };
    for (const char* r : roots) {
        auto found = find_javaw_in(r);
        candidates.insert(candidates.end(), found.begin(), found.end());
    }
    log_line(std::to_string(candidates.size()) + " candidat(s) trouvé(s).");
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la) {
        auto store = find_javaw_in(fs::path(la) /
            "Packages\\Microsoft.4297127D64EC6_8wekyb3d8bbwe\\LocalCache\\Local\\runtime");
        candidates.insert(candidates.end(), store.begin(), store.end());
    }
#else
    // JAVA_HOME d'abord, puis arbos distro. /opt exclu du balayage récursif
    // (trop gros) : en pratique comptent JAVA_HOME, les paquets distro et le
    // JRE embarqué (download_java), plus le java du PATH ci-dessous.
    std::vector<std::string> roots = {"/usr/lib/jvm", "/usr/java"};
    if (const char* jh = std::getenv("JAVA_HOME"); jh && *jh)
        roots.insert(roots.begin(), std::string(jh) + "/bin");
    for (const auto& r : roots) {
        auto found = find_javaw_in(r);
        candidates.insert(candidates.end(), found.begin(), found.end());
    }
    log_line(std::to_string(candidates.size()) + " candidat(s) trouvé(s).");
    if (const char* path = std::getenv("PATH"); path && *path) {
        std::istringstream ps(path);
        std::string d;
        while (std::getline(ps, d, ':')) {
            if (d.empty()) continue;
            const fs::path cand = fs::path(d) / "java";
            std::error_code ec2;
            if (fs::is_regular_file(cand, ec2) &&
                ::access(cand.string().c_str(), X_OK) == 0)
                candidates.push_back(cand.string());
        }
    }
#endif

    struct Cand { std::string path; int major; };
    std::vector<Cand> scored;
    for (auto& c : candidates) {
        const int major = detect_java_major(c);
        if (major >= requiredMajor) scored.push_back({c, major});
    }
    std::sort(scored.begin(), scored.end(), [requiredMajor](const Cand& a, const Cand& b) {
        return (a.major - requiredMajor) < (b.major - requiredMajor);
    });
    std::optional<std::string> result;
    if (!scored.empty()) result = scored.front().path;
    {
        std::lock_guard<std::mutex> g(scan_cache_mutex());
        scan_cache()[key] = result;
    }
    return result;
}

std::optional<std::string> download_java(int major,
                                         const std::function<void(const char*)>& status,
                                         std::atomic<bool>& cancel) {
    try {
        const fs::path runtimeRoot = runtime_root();
        const fs::path jreDir = runtimeRoot / ("jre-" + std::to_string(major));
        const fs::path marker = jreDir / ".done";

        if (fs::exists(marker)) {
            const auto existing = find_javaw_in(jreDir);
            if (!existing.empty()) return existing.front();
        }

        const std::string url =
            "https://api.adoptium.net/v3/binary/latest/" + std::to_string(major) +
#ifdef _WIN32
            "/ga/windows/x64/jre/hotspot/normal/eclipse";
#else
            "/ga/linux/x64/jre/hotspot/normal/eclipse";
#endif
        const fs::path zipPath = runtimeRoot / ("adoptium-jre-" + std::to_string(major) + ".zip");
        std::error_code ec;
        fs::create_directories(runtimeRoot, ec);

        if (!http::get_to_file(url, zipPath, nullptr, &cancel))
            throw std::runtime_error("download adoptium");

        if (status) status("Extraction de Java...");
        fs::remove_all(jreDir, ec);
        if (zip_extract_all(zipPath, jreDir) < 0)
            throw std::runtime_error("extraction jre");
        { std::ofstream m(marker); m << "ok"; }

        const auto found = find_javaw_in(jreDir);
        if (found.empty()) return std::nullopt;
        return found.front();
    } catch (const std::exception& ex) {
        log_line(std::string("Téléchargement Java impossible : ") + ex.what());
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Arguments
// ---------------------------------------------------------------------------

namespace {
void replace_all(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::vector<std::string> split_spaces(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream ss(s);
    std::string tok;
    while (ss >> tok) out.push_back(tok);
    return out;
}
} // namespace

std::vector<std::string> build_jvm_args(const std::string& classpath,
                                        const std::string& natives,
                                        bool isForge,
                                        const std::vector<std::string>* officialJvm,
                                        int ramGb,
                                        const std::string& extraJvmArgs) {
    std::vector<std::string> args;
    const int ram = std::clamp(ramGb, 1, 32);
    args.push_back("-Xmx" + std::to_string(ram) + "G");

    if (officialJvm && !officialJvm->empty()) {
#ifdef _WIN32
        const std::string libDir = runtime_root().string() + "\\libraries";
#else
        const std::string libDir = runtime_root().string() + "/libraries";
#endif
        for (const auto& token : *officialJvm) {
            std::string t = token;
            replace_all(t, "${natives_directory}", natives);
            replace_all(t, "${library_directory}", libDir);
#ifdef _WIN32
            replace_all(t, "${classpath_separator}", ";");
#else
            replace_all(t, "${classpath_separator}", ":");
#endif
            replace_all(t, "${classpath}", classpath);
            replace_all(t, "${launcher_name}", "TeamLauncher");
            replace_all(t, "${launcher_version}", "1.0");
            if (!t.empty()) args.push_back(t);
        }
    } else {
        args.push_back("-Djava.library.path=" + natives);
        args.push_back("-cp");
        args.push_back(classpath);
    }

    if (!extraJvmArgs.empty())
        for (const auto& a : split_spaces(extraJvmArgs)) args.push_back(a);

    if (isForge) {
        args.push_back("-Dsun.java2d.d3d=false");
        args.push_back("-Dsun.java2d.noddraw=true");
        args.push_back("-Dsun.java2d.opengl=true");
        args.push_back("-XX:+UseG1GC");
        args.push_back("-XX:MaxGCPauseMillis=200");
        args.push_back("-XX:+UseStringDeduplication");
        args.push_back("-Xss512k");
        if (ram <= 4) {
            args.push_back("-XX:ParallelGCThreads=2");
            args.push_back("-XX:ConcGCThreads=1");
            args.push_back("-XX:InitialHeapSize=256m");
            args.push_back("-XX:+UseCompressedOops");
        }
        args.push_back("-Dfml.ignoreInvalidMinecraftCertificates=true");
        args.push_back("-Dfml.ignorePatchDiscrepancies=true");
    }
    return args;
}

std::vector<std::string> build_game_args(const std::string& version,
                                         const McSession& s,
                                         const std::string& assetsIndex,
                                         const std::string* legacyArgs,
                                         bool hasModernArgs,
                                         const std::string* joinServer) {
    std::vector<std::string> args;
    const bool modern = hasModernArgs || !legacyArgs || legacyArgs->empty();
    if (modern) {
        args = {"--username", s.name,
                "--version", version,
                "--gameDir", ".",
                "--assetsDir", (runtime_root() / "assets").string(),
                "--assetIndex", assetsIndex,
                "--uuid", s.uuid,
                "--accessToken", s.accessToken,
                "--userType", "msa"};
    } else {
        std::string t = *legacyArgs;
        replace_all(t, "${auth_player_name}", s.name);
        replace_all(t, "${auth_uuid}", s.uuid);
        replace_all(t, "${auth_access_token}", s.accessToken);
        replace_all(t, "${auth_session}", s.accessToken);
        replace_all(t, "${user_type}", "legacy");
        replace_all(t, "${user_properties}", "{}");
        replace_all(t, "${version_name}", version);
        replace_all(t, "${game_directory}", ".");
        replace_all(t, "${game_dir}", ".");
        replace_all(t, "${assets_root}", (runtime_root() / "assets").string());
        replace_all(t, "${assets_index_name}", assetsIndex);
        args = split_spaces(t);
    }

    if (joinServer && !joinServer->empty()) {
        const size_t colon = joinServer->find(':');
        args.push_back("--server");
        args.push_back(joinServer->substr(0, colon));
        if (colon != std::string::npos) {
            args.push_back("--port");
            args.push_back(joinServer->substr(colon + 1));
        }
    }
    return args;
}

std::optional<std::string> latest_release() {
    const auto body = http::get_string(
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
    if (!body) return std::nullopt;
    try {
        const auto doc = nlohmann::json::parse(*body);
        return doc.at("latest").at("release").get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------
// Processus de jeu
// ---------------------------------------------------------------------------

namespace {
#ifdef _WIN32
std::wstring quote_arg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : a) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        if (ch == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out += L'"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, L'\\');
        backslashes = 0;
        out += ch;
    }
    out.append(backslashes * 2, L'\\');
    out += L'"';
    return out;
}
#endif // _WIN32 (cote POSIX : argv direct, pas de quoting)
} // namespace

std::optional<GameProcess> start_game(const LaunchInput& in) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return std::nullopt;
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&errR, &errW, &sa, 0)) {
        CloseHandle(outR); CloseHandle(outW);
        return std::nullopt;
    }
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmd = quote_arg(utf8_to_wide(in.javaExe));
    for (const auto& a : in.args)
        cmd += L" " + quote_arg(utf8_to_wide(a));

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = errW;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};

    const std::wstring cwd = utf8_to_wide(in.gameDir);
    const BOOL ok = CreateProcessW(utf8_to_wide(in.javaExe).c_str(), cmdBuf.data(),
                                   nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, cwd.empty() ? nullptr : cwd.c_str(),
                                   &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(outR); CloseHandle(errR);
        return std::nullopt;
    }
    CloseHandle(pi.hThread);

    GameProcess g;
    g.hProcess = pi.hProcess;
    g.pid = pi.dwProcessId;
    g.outRead = outR;
    g.errRead = errR;
    return g;
#else
    // argv direct : pas de quoting (posix_spawn ne passe par aucun shell).
    // Les fds sont stockés en void* (même layout que les HANDLE Windows).
    std::vector<std::string> argv;
    argv.reserve(in.args.size() + 1);
    argv.push_back(in.javaExe);
    argv.insert(argv.end(), in.args.begin(), in.args.end());
    auto child = proc::spawn(in.javaExe, argv, in.gameDir, /*mergeErr=*/false);
    if (!child) return std::nullopt;
    GameProcess g;
    g.hProcess = reinterpret_cast<void*>(static_cast<intptr_t>(child->pid));
    g.pid = static_cast<unsigned long>(child->pid);
    g.outRead = reinterpret_cast<void*>(static_cast<intptr_t>(child->outFd));
    g.errRead = reinterpret_cast<void*>(static_cast<intptr_t>(child->errFd));
    return g;
#endif
}

void close_game(GameProcess& g) {
#ifdef _WIN32
    const bool dbg = std::getenv("TL_DEBUG_SHUTDOWN") != nullptr;
    // outRead/errRead : PAS de CloseHandle — un pump (thread detache) est en
    // attente de ReadFile dessus ; le noyau retient l'objet pipe tant que le
    // jeu vit et CloseHandle attendrait indfiniment. Le handle est referme a
    // la fin du process launcher ; le pump recoit un EOF naturel si le jeu
    // se termine avant.
    if (dbg) { std::fprintf(stderr, "CG: outRead/errRead skipped\n"); std::fflush(stderr); }
    g.outRead = nullptr;
    g.errRead = nullptr;
    if (dbg) { std::fprintf(stderr, "CG: hProcess\n"); std::fflush(stderr); }
    if (g.hProcess) { CloseHandle(static_cast<HANDLE>(g.hProcess)); g.hProcess = nullptr; }
    if (dbg) { std::fprintf(stderr, "CG: done\n"); std::fflush(stderr); }
#else
    // Comme Windows : on ne ferme PAS outRead/errRead (pumps en read()
    // bloquant ; fermer un fd pendant un read() est une course). Fds refermés
    // à la fin du process launcher. Pas de handle process à fermer (waitpid
    // moissonne par pid).
    g.outRead = nullptr;
    g.errRead = nullptr;
    g.hProcess = nullptr;
#endif
}

int wait_game(GameProcess& g) {
#ifdef _WIN32
    if (!g.hProcess) return -1;
    WaitForSingleObject(static_cast<HANDLE>(g.hProcess), INFINITE);
    DWORD code = 1;
    if (!GetExitCodeProcess(static_cast<HANDLE>(g.hProcess), &code)) return -1;
    return static_cast<int>(code);
#else
    if (!g.hProcess) return -1;
    return proc::wait_exit(static_cast<pid_t>(reinterpret_cast<intptr_t>(g.hProcess)),
                           /*timeoutMs=*/-1);
#endif
}

void start_game_log_writer(GameProcess& g, const fs::path& gameLog) {
    struct Sink {
        std::mutex m;
        std::ofstream out;
    };
    auto sink = std::make_shared<Sink>();
    std::error_code ec;
    if (gameLog.has_parent_path()) fs::create_directories(gameLog.parent_path(), ec);
    sink->out.open(gameLog, std::ios::app);
    if (!sink->out) return;

#ifdef _WIN32
    auto pump = [sink](HANDLE h) {
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0) {
            std::lock_guard<std::mutex> g(sink->m);
            sink->out.write(buf, read);
            sink->out.flush();
        }
    };
    const HANDLE o = static_cast<HANDLE>(g.outRead);
    const HANDLE e = static_cast<HANDLE>(g.errRead);
#else
    auto pump = [sink](int fd) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            std::lock_guard<std::mutex> g(sink->m);
            sink->out.write(buf, n);
            sink->out.flush();
        }
    };
    const int o = static_cast<int>(reinterpret_cast<intptr_t>(g.outRead));
    const int e = static_cast<int>(reinterpret_cast<intptr_t>(g.errRead));
#endif
    if (o) std::thread(pump, o).detach();
    if (e) std::thread(pump, e).detach();
}

} // namespace tl

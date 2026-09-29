#include "game_installer.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp"
#include "http_win.hpp"
#include "proc.hpp" // posix_spawn (POSIX) ; vide sous Windows
#include "util_hash.hpp"
#include "util_parallel.hpp"
#include "util_zip.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
// Etape 5 (Linux) : XDG, strcasecmp/read, localtime_r.
#include <strings.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl {

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

fs::path runtime_root() {
    if (const char* td = std::getenv("TL_RUNTIME_DIR"); td && *td)
        return fs::path(td);
#ifdef _WIN32
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
        return fs::path(la) / "TeamLauncher" / "runtime";
#else
    // Même racine XDG que DataStore (sans en dépendre).
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return fs::path(xdg) / "TeamLauncher" / "runtime";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".local" / "share" / "TeamLauncher" / "runtime";
#endif
    return fs::current_path() / "runtime";
}

namespace {
fs::path versions_dir() { return runtime_root() / "versions"; }
fs::path libraries_dir() { return runtime_root() / "libraries"; }
fs::path assets_dir() { return runtime_root() / "assets"; }
fs::path natives_dir(const std::string& v) { return runtime_root() / "natives" / v; }

void check_cancel(const std::atomic<bool>& cancel) {
    if (cancel.load(std::memory_order_relaxed)) throw CancelledError();
}

void sleep_cancellable(int ms, const std::atomic<bool>& cancel) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < until) {
        check_cancel(cancel);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

// DownloadAsync du C# : retries x3 (500*attempt), sha1 optionnel, fast = on fait
// confiance a un fichier present, quiet = echec final rend false sinon throw.
bool download_file(const std::string& url, const fs::path& dest,
                   const std::string* sha1, std::atomic<bool>* cancel,
                   bool quiet, bool fast) {
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        if (fast) return true;
        if (!sha1) return true;
        if (const auto h = sha1_hex(dest); h && *h == *sha1) return true;
    }
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (http::get_to_file(url, dest, nullptr, cancel, 1)) {
            if (!sha1) return true;
            if (const auto h = sha1_hex(dest); h && *h == *sha1) return true;
            fs::remove(dest, ec); // SHA1 mismatch -> retentative
        }
        if (cancel && cancel->load(std::memory_order_relaxed)) throw CancelledError();
        if (attempt < 3) {
            if (cancel)
                sleep_cancellable(500 * attempt, *cancel);
            else
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * attempt));
        }
    }
    if (quiet) return false;
    throw std::runtime_error("Échec de téléchargement : " + url);
}

std::string now_iso() {
    const auto t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm);
    return buf;
}

void ensure_launcher_profiles() {
    const fs::path p = runtime_root() / "launcher_profiles.json";
    std::error_code ec;
    if (!fs::exists(p, ec)) {
        std::ofstream out(p);
        out << "{\"profiles\":{},\"settings\":{}}";
    }
}

bool iequals(const std::string& a, const char* b) {
#ifdef _WIN32
    return _stricmp(a.c_str(), b) == 0;
#else
    return ::strcasecmp(a.c_str(), b) == 0;
#endif
}

} // namespace

// Lance un installeur .jar (Forge/NeoForge) : java -jar jar --installClient <runtime>.
// Sortie stdout+stderr concatenee (lecteurs paralleles anti-deadlock).
// Expose (header) pour les tests ; le reste du namespace reste local.
std::string run_jar_installer(const std::string& java, const fs::path& installerJar) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE outR = nullptr, outW = nullptr, errR = nullptr, errW = nullptr;
    if (!CreatePipe(&outR, &outW, &sa, 0)) return {};
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    if (!CreatePipe(&errR, &errW, &sa, 0)) {
        CloseHandle(outR); CloseHandle(outW);
        return {};
    }
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);

    auto wide = [](const fs::path& p) {
        const std::wstring w = p.wstring();
        return w;
    };
    std::wstring cmd = L"\"" + wide(fs::path(java)) + L"\" -jar \"" +
                       wide(installerJar) + L"\" --installClient \"" +
                       wide(runtime_root()) + L"\"";
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outW;
    si.hStdError = errW;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(outW);
    CloseHandle(errW);
    if (!ok) {
        CloseHandle(outR); CloseHandle(errR);
        return {};
    }
    CloseHandle(pi.hThread);

    std::string outText, errText;
    auto pump = [](HANDLE h, std::string* dst) {
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(h, buf, sizeof(buf), &read, nullptr) && read > 0)
            dst->append(buf, read);
    };
    std::thread t1(pump, outR, &outText);
    std::thread t2(pump, errR, &errText);
    WaitForSingleObject(pi.hProcess, INFINITE);
    t1.join();
    t2.join();
    CloseHandle(outR);
    CloseHandle(errR);
    CloseHandle(pi.hProcess);
    return outText + errText;
#else
    // argv direct (pas de ligne à parser) ; pompes parallèles anti-deadlock
    // comme côté Windows (un .jar bavard > 64 Kio bloquerait sinon).
    auto child = proc::spawn(java, {java, "-jar", installerJar.string(),
                                    "--installClient", runtime_root().string()},
                             "", false);
    if (!child) return {};
    std::string outText, errText;
    auto pump = [](int fd, std::string* dst) {
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(fd, buf, sizeof(buf));
            if (n <= 0) break;
            dst->append(buf, static_cast<size_t>(n));
        }
    };
    std::thread t1(pump, child->outFd, &outText);
    std::thread t2(pump, child->errFd, &errText);
    proc::wait_exit(child->pid, -1);
    t1.join();
    t2.join();
    proc::close_fd(child->outFd);
    proc::close_fd(child->errFd);
    return outText + errText;
#endif
}

namespace {

void append_install_log(const std::string& header, const std::string& log) {
    const fs::path p = runtime_root() / "forge-install.log";
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::app);
    out << header << "\n" << log << "\n";
}
} // namespace

// ---------------------------------------------------------------------------
// Regles / Maven / JVM args (exposes)
// ---------------------------------------------------------------------------

bool rules_allow(const json& lib) {
    if (!lib.contains("rules")) return true;
#ifdef _WIN32
    constexpr const char* kOsName = "windows";
#else
    constexpr const char* kOsName = "linux";
#endif
    bool allowed = false;
    for (const auto& rule : lib.at("rules")) {
        bool applies = true;
        if (rule.contains("os")) {
            const auto& os = rule.at("os");
            applies = !os.contains("name") || os.at("name").get<std::string>() == kOsName;
        }
        if (applies)
            allowed = rule.at("action").get<std::string>() == "allow";
    }
    return allowed;
}

std::string maven_name_to_path(const std::string& name) {
    if (name.find('@') != std::string::npos) return {};
    std::vector<std::string> parts;
    std::istringstream ss(name);
    std::string p;
    while (std::getline(ss, p, ':')) parts.push_back(p);
    if (parts.size() < 3) return {};
    std::string group = parts[0];
#ifdef _WIN32
    std::replace(group.begin(), group.end(), '.', '\\');
#else
    std::replace(group.begin(), group.end(), '.', '/');
#endif
    const std::string& artifact = parts[1];
    const std::string& ver = parts[2];
    const std::string classifier = parts.size() > 3 ? "-" + parts[3] : "";
    fs::path rel = fs::path(group) / artifact / ver /
                   (artifact + "-" + ver + classifier + ".jar");
    return rel.string();
}

std::vector<std::string> extract_jvm_args(const json& root) {
    std::vector<std::string> list;
    if (!root.contains("arguments")) return list;
    const auto& args = root.at("arguments");
    if (!args.is_object() || !args.contains("jvm") || !args.at("jvm").is_array())
        return list;
    for (const auto& el : args.at("jvm")) {
        if (el.is_string()) {
            list.push_back(el.get<std::string>());
            continue;
        }
        if (el.is_object() && rules_allow(el)) {
            const auto& v = el.at("value");
            if (v.is_string())
                list.push_back(v.get<std::string>());
            else if (v.is_array())
                for (const auto& s : v)
                    if (s.is_string()) list.push_back(s.get<std::string>());
        }
    }
    return list;
}

// ---------------------------------------------------------------------------
// JSON de version
// ---------------------------------------------------------------------------

namespace {
json get_version_json(const std::string& versionId, std::atomic<bool>& cancel) {
    const fs::path localPath = versions_dir() / versionId / (versionId + ".json");
    std::error_code ec;
    if (fs::exists(localPath, ec)) {
        std::ifstream in(localPath, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        return json::parse(ss.str());
    }

    const auto manifestBody = http::get_string(
        "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json", &cancel);
    if (!manifestBody) throw std::runtime_error("manifeste Mojang indisponible");
    const auto manifest = json::parse(*manifestBody);

    for (const auto& v : manifest.at("versions")) {
        if (v.at("id").get<std::string>() != versionId) continue;
        std::error_code ec2;
        fs::create_directories(localPath.parent_path(), ec2);
        download_file(v.at("url").get<std::string>(), localPath, nullptr, &cancel,
                      /*quiet=*/true, /*fast=*/false);
        std::ifstream in(localPath, std::ios::binary);
        if (!in) throw std::runtime_error("version json illisible : " + versionId);
        std::ostringstream ss;
        ss << in.rdbuf();
        return json::parse(ss.str());
    }
    throw std::runtime_error("Version « " + versionId + " » introuvable chez Mojang.");
}
} // namespace

// ---------------------------------------------------------------------------
// Install
// ---------------------------------------------------------------------------

nlohmann::json install(const std::string& versionId, const std::string& loader,
                       InstallProgress progress, std::atomic<bool>& cancel,
                       bool forceVerify) {
    std::mutex pm;
    auto P = [&](const char* stage, int done, int total) {
        if (!progress) return;
        std::lock_guard<std::mutex> g(pm);
        progress(stage, done, total);
    };

    std::error_code ec;
    fs::create_directories(versions_dir(), ec);
    fs::create_directories(libraries_dir(), ec);
    fs::create_directories(assets_dir(), ec);

    const fs::path markerPath = versions_dir() / versionId / ".tl-verified";
    if (forceVerify) fs::remove(markerPath, ec);
    bool fast = fs::exists(markerPath, ec);

    // ---- 1. JSON de la version ----
    P(fast ? "Lecture du cache" : "Recherche de la version", 0, 1);
    const json vjson = get_version_json(versionId, cancel);
    const fs::path vDir = versions_dir() / versionId;
    fs::create_directories(vDir, ec);
    const fs::path jarPath = vDir / (versionId + ".jar");

    // ---- 2. Client jar ----
    if (!fast) {
        P("Téléchargement du jeu", 0, 1);
        if (vjson.contains("downloads") && vjson.at("downloads").contains("client")) {
            const auto& client = vjson.at("downloads").at("client");
            const std::string sha = client.value("sha1", "");
            download_file(client.at("url").get<std::string>(), jarPath,
                          sha.empty() ? nullptr : &sha, &cancel, false, false);
        }
    } else if (!fs::exists(jarPath, ec)) {
        fast = false; // jar disparu : mode complet
    }

    // ---- 3. Bibliotheques + natives (parallele x8) ----
    std::vector<std::string> classpathEntries;
    if (vjson.contains("libraries")) {
        std::vector<const json*> libs;
        for (const auto& l : vjson.at("libraries"))
            if (rules_allow(l)) libs.push_back(&l);

        std::mutex bagM;
        std::vector<std::string> bag;
        std::atomic<int> done{0};
        const int total = static_cast<int>(libs.size());

        try {
            parallel_for(total, 8, [&](int i) {
                check_cancel(cancel);
                try {
                    const json& lib = *libs[i];
                    if (lib.contains("downloads")) {
                        const auto& ld = lib.at("downloads");
                        if (ld.contains("artifact")) {
                            const auto& art = ld.at("artifact");
                            std::string relPath = art.at("path").get<std::string>();
                            std::replace(relPath.begin(), relPath.end(), '/', '\\');
                            const fs::path dest = libraries_dir() / fs::path(relPath);
                            const std::string sha = art.value("sha1", "");
                            if (!fast || !fs::exists(dest, ec)) {
                                if (!download_file(art.at("url").get<std::string>(), dest,
                                                   sha.empty() ? nullptr : &sha, &cancel,
                                                   false, fast))
                                    throw std::runtime_error("lib: " + relPath);
                            }
                            std::lock_guard<std::mutex> g(bagM);
                            bag.push_back(dest.string());
                        }
                        if (ld.contains("classifiers")) {
                            const auto& cls = ld.at("classifiers");
#ifdef _WIN32
                            constexpr const char* kNativesKey = "natives-windows";
#else
                            constexpr const char* kNativesKey = "natives-linux";
#endif
                            if (cls.contains(kNativesKey)) {
                                const auto& nat = cls.at(kNativesKey);
                                std::string natRel = nat.at("path").get<std::string>();
#ifdef _WIN32
                                std::replace(natRel.begin(), natRel.end(), '/', '\\');
#endif
                                const fs::path natDest = libraries_dir() / fs::path(natRel);
                                const std::string sha = nat.value("sha1", "");
                                // C# : skip download si fast+present ; on force si absent
                                if (!fs::exists(natDest, ec) || !fast)
                                    download_file(nat.at("url").get<std::string>(), natDest,
                                                  sha.empty() ? nullptr : &sha, &cancel,
                                                  false, fast);
                                zip_extract_natives(natDest, natives_dir(versionId));
                            }
                        }
                    }
                } catch (...) {
                    const int d = done.fetch_add(1) + 1;
                    P("Bibliothèques", d, total);
                    throw;
                }
                const int d = done.fetch_add(1) + 1;
                P("Bibliothèques", d, total);
            }, &cancel);
        } catch (const CancelledError&) {
            throw;
        }
        std::sort(bag.begin(), bag.end());
        classpathEntries = std::move(bag);
    }

    // ---- 4. Assets (parallele x16, sautes en mode rapide) ----
    const std::string assetsIndexName =
        vjson.contains("assets") ? vjson.at("assets").get<std::string>() : versionId;
    const fs::path objectsDir = assets_dir() / "objects";
    const bool assetsAlreadyThere =
        fast && fs::exists(objectsDir, ec) && !fs::is_empty(objectsDir, ec);
    std::atomic<int> failedAssets{0};
    bool assetsHadFailures = false;

    if (vjson.contains("assetIndex") && !assetsAlreadyThere) {
        const fs::path indexPath =
            assets_dir() / "indexes" / (assetsIndexName + ".json");
        fs::create_directories(indexPath.parent_path(), ec);
        download_file(vjson.at("assetIndex").at("url").get<std::string>(), indexPath,
                      nullptr, &cancel, false, false);

        std::ifstream iin(indexPath, std::ios::binary);
        std::ostringstream iss;
        iss << iin.rdbuf();
        const auto index = json::parse(iss.str());

        std::vector<std::string> hashes;
        for (const auto& [name, obj] : index.at("objects").items())
            hashes.push_back(obj.at("hash").get<std::string>());

        std::atomic<int> done{0};
        const int total = static_cast<int>(hashes.size());

        parallel_for(total, 16, [&](int i) {
            const std::string& hash = hashes[i];
            try {
                const std::string sub = hash.substr(0, 2);
                const fs::path dest = objectsDir / sub / hash;
                if (!fs::exists(dest, ec)) {
                    if (!download_file("https://resources.download.minecraft.net/" + sub +
                                           "/" + hash,
                                       dest, &hash, &cancel, /*quiet=*/true, false))
                        failedAssets.fetch_add(1);
                }
                if (!fs::exists(dest, ec)) failedAssets.fetch_add(1);
            } catch (...) {
                failedAssets.fetch_add(1);
            }
            const int d = done.fetch_add(1) + 1;
            P("Assets du jeu", d, total);
        }, &cancel);

        if (failedAssets.load() > 0) {
            assetsHadFailures = true;
            P("Seconde passe assets...", 0, total);
            for (const auto& hash : hashes) {
                check_cancel(cancel);
                const std::string sub = hash.substr(0, 2);
                const fs::path dest = objectsDir / sub / hash;
                if (!fs::exists(dest, ec)) {
                    try {
                        download_file("https://resources.download.minecraft.net/" + sub +
                                          "/" + hash,
                                      dest, &hash, &cancel, /*quiet=*/false, false);
                    } catch (...) {
                    }
                }
            }
        }
    }

    if (!fs::exists(markerPath, ec) && !assetsHadFailures) {
        std::ofstream m(markerPath);
        m << now_iso();
    }

    auto make_info = [&](json root, const std::vector<std::string>& cp,
                         int javaMajor, bool forge, const std::string* legacyOverride) {
        json info;
        info["mainClass"] = root.at("mainClass");
        info["classpath"] = cp;
        info["jar"] = jarPath.string();
        info["natives"] = natives_dir(versionId).string();
        info["assetsIndex"] = assetsIndexName;
        info["javaMajor"] = javaMajor;
        info["minecraftArguments"] =
            legacyOverride ? json(*legacyOverride)
            : root.contains("minecraftArguments") ? root.at("minecraftArguments")
                                                  : json(nullptr);
        info["hasArguments"] = root.contains("arguments");
        const auto jvm = extract_jvm_args(root);
        info["jvmArgs"] = jvm.empty() ? json(nullptr) : json(jvm);
        if (forge) info["isForge"] = forge; // presence de la cle = args Forge (C#)
        return info;
    };

    auto dedup_merge = [](std::vector<std::string> base,
                          const std::vector<std::string>& extra) {
        for (const auto& e : extra)
            if (std::find(base.begin(), base.end(), e) == base.end())
                base.push_back(e);
        return base;
    };

    const int vanillaJava =
        vjson.contains("javaVersion") ? vjson.at("javaVersion").at("majorVersion").get<int>()
                                      : 8;

    // ---- 5. NeoForge ----
    if (iequals(loader, "NeoForge")) {
        P("Installation de NeoForge", 0, 0);
        const std::string neoId = ensure_neoforge_installed(versionId, cancel);

        const fs::path neoJsonPath = versions_dir() / neoId / (neoId + ".json");
        std::ifstream nin(neoJsonPath, std::ios::binary);
        std::ostringstream nss;
        nss << nin.rdbuf();
        const json nroot = json::parse(nss.str());

        std::vector<std::string> neoCp;
        if (nroot.contains("libraries")) {
            for (const auto& lib : nroot.at("libraries")) {
                if (!lib.contains("name")) continue;
                const std::string rel =
                    maven_name_to_path(lib.at("name").get<std::string>());
                if (rel.empty()) continue;
                const fs::path full = libraries_dir() / fs::path(rel);
                if (!fs::exists(full, ec) && lib.contains("downloads") &&
                    lib.at("downloads").contains("artifact")) {
                    const auto& art = lib.at("downloads").at("artifact");
                    const std::string sha = art.value("sha1", "");
                    download_file(art.at("url").get<std::string>(), full,
                                  sha.empty() ? nullptr : &sha, &cancel, true, false);
                }
                if (fs::exists(full, ec)) neoCp.push_back(full.string());
            }
        }
        auto cp = dedup_merge(std::move(neoCp), classpathEntries);
        const int javaNeed = vanillaJava;
        json info = make_info(nroot, cp, std::max(javaNeed, 17), false, nullptr);
        info["isForge"] = false; // cle PRESENTE (C#) : GameLauncher lit la presence
        return info;
    }

    // ---- 5ter. Forge ----
    if (iequals(loader, "Forge")) {
        P("Installation de Forge", 0, 0);
        const std::string forgeId = ensure_forge_installed(versionId, cancel);

        const fs::path forgeJsonPath = versions_dir() / forgeId / (forgeId + ".json");
        std::ifstream fin(forgeJsonPath, std::ios::binary);
        std::ostringstream fss;
        fss << fin.rdbuf();
        const json root = json::parse(fss.str());

        std::vector<json> libElements;
        auto collect = [&libElements](const json& el) {
            if (!el.contains("libraries")) return;
            for (const auto& lib : el.at("libraries"))
                if (lib.contains("name")) libElements.push_back(lib);
        };
        collect(root);
        const fs::path profilePath =
            versions_dir() / forgeId / "install_profile.json";
        if (fs::exists(profilePath, ec)) {
            std::ifstream pin(profilePath, std::ios::binary);
            std::ostringstream pss;
            pss << pin.rdbuf();
            collect(json::parse(pss.str()));
        }

        // Dedoublonnage par nom Maven (ordre d'insertion)
        std::set<std::string> seen;
        std::vector<json> unique;
        for (auto& l : libElements) {
            if (seen.insert(l.at("name").get<std::string>()).second)
                unique.push_back(std::move(l));
        }

        std::vector<std::string> forgeCp{
            (versions_dir() / forgeId / (forgeId + ".jar")).string()};
        int done = 0;
        const int total = static_cast<int>(unique.size());
        for (const auto& lib : unique) {
            if (lib.contains("clientreq") && !lib.at("clientreq").get<bool>()) continue;
            const std::string name = lib.at("name").get<std::string>();
            const std::string rel = maven_name_to_path(name);
            if (rel.empty()) continue;
            const fs::path full = libraries_dir() / fs::path(rel);
            if (!fs::exists(full, ec)) {
                std::string urlBase = lib.contains("url")
                    ? lib.at("url").get<std::string>()
                    : "https://libraries.minecraft.net";
                while (!urlBase.empty() && urlBase.back() == '/') urlBase.pop_back();
                std::string urlPath = rel;
                std::replace(urlPath.begin(), urlPath.end(), '\\', '/');
                try {
                    download_file(urlBase + "/" + urlPath, full, nullptr, &cancel,
                                  true, false);
                } catch (const CancelledError&) {
                    throw;
                } catch (...) {
                }
            }
            if (!fs::exists(full, ec))
                throw std::runtime_error("Bibliothèque Forge manquante : " + name);
            forgeCp.push_back(full.string());
            P("Bibliothèques Forge", ++done, total);
        }

        auto cp = dedup_merge(std::move(forgeCp), classpathEntries);
        return make_info(root, cp, 8, true, nullptr);
    }

    // ---- 5bis. Fabric ----
    if (iequals(loader, "Fabric")) {
        P("Installation de Fabric", 0, 0);
        const std::string fabricId = ensure_fabric_installed(versionId, cancel);
        const fs::path fabricJsonPath =
            versions_dir() / fabricId / (fabricId + ".json");
        std::ifstream fin(fabricJsonPath, std::ios::binary);
        std::ostringstream fss;
        fss << fin.rdbuf();
        const json froot = json::parse(fss.str());

        std::vector<std::string> fabricCp;
        if (froot.contains("libraries")) {
            for (const auto& lib : froot.at("libraries")) {
                if (!lib.contains("name")) continue;
                const std::string rel =
                    maven_name_to_path(lib.at("name").get<std::string>());
                if (rel.empty()) continue;
                const fs::path full = libraries_dir() / fs::path(rel);
                if (!fs::exists(full, ec)) {
                    std::string urlBase = lib.contains("url")
                        ? lib.at("url").get<std::string>()
                        : "https://libraries.minecraft.net";
                    while (!urlBase.empty() && urlBase.back() == '/') urlBase.pop_back();
                    std::string urlPath = rel;
                    std::replace(urlPath.begin(), urlPath.end(), '\\', '/');
                    try {
                        download_file(urlBase + "/" + urlPath, full, nullptr, &cancel,
                                      true, false);
                    } catch (const CancelledError&) {
                        throw;
                    } catch (...) {
                    }
                }
                if (fs::exists(full, ec)) fabricCp.push_back(full.string());
            }
        }

        std::vector<std::string> fabricFinal{jarPath.string()};
        for (const auto& l : fabricCp) fabricFinal.push_back(l);
        auto cp = dedup_merge(std::move(fabricFinal), classpathEntries);

        json info = make_info(froot, cp,
                              vjson.contains("javaVersion") ? vanillaJava : 8,
                              false, nullptr);
        // C# : jvmArgs = vanilla sinon fabric
        const auto vanillaJvm = extract_jvm_args(vjson);
        if (!vanillaJvm.empty()) info["jvmArgs"] = vanillaJvm;
        info["isForge"] = false; // cle PRESENTE (C#) : GameLauncher lit la presence
        return info;
    }

    // ---- Vanilla ----
    std::string legacy;
    const std::string* legacyPtr = nullptr;
    if (vjson.contains("minecraftArguments")) {
        legacy = vjson.at("minecraftArguments").get<std::string>();
        legacyPtr = &legacy;
    }
    json info = make_info(vjson, classpathEntries, vanillaJava, false, legacyPtr);
    return info;
}

// ---------------------------------------------------------------------------
// Installeurs de loaders
// ---------------------------------------------------------------------------

std::string ensure_neoforge_installed(const std::string& mcVersion,
                                      std::atomic<bool>& cancel) {
    // 1. Versions disponibles (ex: "21.1.77" -> MC 1.21.1)
    const auto meta = http::get_string(
        "https://maven.neoforged.net/api/maven/versions/releases/net/neoforged/neoforge",
        &cancel);
    if (!meta) throw std::runtime_error("maven.neoforged.net injoignable");
    const auto listDoc = json::parse(*meta);
    const std::string prefix = mcVersion.size() > 2 ? mcVersion.substr(2) : mcVersion;

    std::string build;
    for (const auto& v : listDoc.at("versions")) {
        if (!v.is_string()) continue;
        const std::string s = v.get<std::string>();
        if (s.rfind(prefix, 0) != 0) continue;
        if (s.find('-') != std::string::npos) continue;
        if (s > build) build = s; // OrderByDescending ordinal
    }
    if (build.empty())
        throw std::runtime_error("NeoForge n'est pas disponible pour Minecraft " +
                                 mcVersion + ".");

    const std::string neoId = "neoforge-" + build;
    const fs::path jsonPath = versions_dir() / neoId / (neoId + ".json");
    std::error_code ec;
    if (fs::exists(jsonPath, ec)) return neoId;

    // 2. Installeur officiel
    fs::create_directories(runtime_root(), ec);
    const fs::path installerJar =
        runtime_root() / ("neoforge-installer-" + build + ".jar");
    const std::string installerUrl =
        "https://maven.neoforged.net/releases/net/neoforged/neoforge/" + build +
        "/neoforge-" + build + "-installer.jar";
    download_file(installerUrl, installerJar, nullptr, &cancel, false, false);

    // 3. Installation silencieuse (Java 17+ requis)
    const auto java = find_java(17);
    if (!java)
        throw std::runtime_error("Java 17+ est requis pour installer NeoForge.");
    ensure_launcher_profiles();

    const std::string log = run_jar_installer(*java, installerJar);
    append_install_log("--- neoforge " + build + " ---", log);

    if (!fs::exists(jsonPath, ec))
        throw std::runtime_error(
            "L'installation de NeoForge a échoué (voir runtime"
#ifdef _WIN32
            "\\forge-install.log).");
#else
            "/forge-install.log).");
#endif
    return neoId;
}

std::string ensure_fabric_installed(const std::string& mcVersion,
                                    std::atomic<bool>& cancel) {
    // 1. Derniere version du loader Fabric compatible
    const auto loaderList = http::get_string(
        "https://meta.fabricmc.net/v2/versions/loader/" + mcVersion, &cancel);
    if (!loaderList) throw std::runtime_error("meta.fabricmc.net injoignable");
    const auto list = json::parse(*loaderList);
    if (!list.is_array() || list.empty())
        throw std::runtime_error("Fabric n'est pas disponible pour Minecraft " +
                                 mcVersion + ".");
    const std::string loaderVer = list[0].at("loader").at("version").get<std::string>();

    const std::string fabricId =
        "fabric-loader-" + loaderVer + "-" + mcVersion;
    const fs::path jsonPath = versions_dir() / fabricId / (fabricId + ".json");
    std::error_code ec;
    if (fs::exists(jsonPath, ec)) return fabricId;

    // 2. Profil de version officiel (format standard Mojang)
    fs::create_directories(jsonPath.parent_path(), ec);
    const auto profile = http::get_string(
        "https://meta.fabricmc.net/v2/versions/loader/" + mcVersion + "/" +
        loaderVer + "/profile/json",
        &cancel);
    if (!profile) throw std::runtime_error("profil Fabric indisponible");
    std::ofstream out(jsonPath, std::ios::binary);
    out << *profile;
    return fabricId;
}

std::string ensure_forge_installed(const std::string& mcVersion,
                                   std::atomic<bool>& cancel) {
    // 1. Derniere build de Forge (promotions officielles)
    const auto promosJson = http::get_string(
        "https://files.minecraftforge.net/net/minecraftforge/forge/promotions_slim.json",
        &cancel);
    if (!promosJson) throw std::runtime_error("promotions Forge injoignables");
    const auto promos = json::parse(*promosJson);
    const auto& p = promos.at("promos");
    std::string build;
    const std::string recKey = mcVersion + "-recommended";
    const std::string latKey = mcVersion + "-latest";
    if (p.contains(recKey))
        build = p.at(recKey).get<std::string>();
    else if (p.contains(latKey))
        build = p.at(latKey).get<std::string>();
    else
        throw std::runtime_error(
            "Aucune version de Forge n'existe pour Minecraft " + mcVersion + ".");

    const std::string forgeId = mcVersion + "-forge-" + build;
    const fs::path installerJar = runtime_root() /
        ("forge-installer-" + mcVersion + "-" + build + ".jar");
    const fs::path forgeJsonPath = versions_dir() / forgeId / (forgeId + ".json");
    std::error_code ec;

    // 2. Pas encore installee ? Telechargement de l'installeur + installation
    const auto installerUrl =
        "https://maven.minecraftforge.net/net/minecraftforge/forge/" + mcVersion +
        "-" + build + "/forge-" + mcVersion + "-" + build + "-installer.jar";

    if (!fs::exists(forgeJsonPath, ec)) {
        download_file(installerUrl, installerJar, nullptr, &cancel, false, false);

        // L'installeur exige un profil de launcher dans le dossier cible
        fs::create_directories(runtime_root(), ec);
        ensure_launcher_profiles();

        const auto java = find_java(8);
        if (!java)
            throw std::runtime_error(
                "Java 8 est requis pour installer Forge sur les anciennes versions.");
        const std::string log = run_jar_installer(*java, installerJar);
        append_install_log("--- " + forgeId + " ---", log);

        if (!fs::exists(forgeJsonPath, ec))
            throw std::runtime_error(
                "L'installation de Forge a échoué (voir runtime"
#ifdef _WIN32
                "\\forge-install.log).");
#else
                "/forge-install.log).");
#endif
    }

    // 3. install_profile.json = liste COMPLETE des bibliotheques (le JSON de
    //    version est incomplet) — extrait meme si deja installe.
    const fs::path profilePath = versions_dir() / forgeId / "install_profile.json";
    if (!fs::exists(profilePath, ec)) {
        if (!fs::exists(installerJar, ec))
            download_file(installerUrl, installerJar, nullptr, &cancel, false, false);
        zip_extract_entry(installerJar, "install_profile.json", profilePath);
    }
    return forgeId;
}

} // namespace tl

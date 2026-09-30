#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#else
// Etape 5 (Linux) : XDG pour les chemins, getrandom() pour l'aleatoire,
// rename() pour l'ecriture atomique. Plus de dependance Win32 ici.
#include <strings.h>    // strcasecmp
#include <sys/random.h> // getrandom
#include <unistd.h>     // readlink (/proc/self/exe)
#endif

#include "datastore.hpp"

#include "secrets.hpp" // chiffrement DPAPI des champs sensibles de config.json
#include "obf.hpp"      // webhook Discord obfusque (S3)

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl {

// Comparaison insensible a la casse : _stricmp (Win32) / strcasecmp (POSIX).
static int strCaseCmp(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return ::strcasecmp(a, b);
#endif
}

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const noexcept {
    return strCaseCmp(a.c_str(), b.c_str()) < 0;
}

static std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

static bool equalsIgnoreCase(const std::string& a, const char* b) {
    return strCaseCmp(a.c_str(), b) == 0;
}

static fs::path exeDir() {
#ifdef _WIN32
    wchar_t buf[4096];
    DWORD n = GetModuleFileNameW(nullptr, buf, 4096);
    if (n == 0 || n >= 4096) return fs::current_path();
    return fs::path(buf).parent_path();
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return fs::current_path();
    buf[n] = '\0';
    return fs::path(buf).parent_path();
#endif
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string newInstallationId() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  static_cast<unsigned long long>(dist(gen)),
                  static_cast<unsigned long long>(dist(gen)));
    return std::string(buf, 32);
}

// ---------------------------------------------------------------------------
// Debouncer (equiv. System.Threading.Timer 500 ms de DataStore.Save)
// ---------------------------------------------------------------------------

namespace {

class Debouncer {
public:
    void request() {
        {
            std::lock_guard<std::mutex> g(m_);
            pending_ = true;
            ensureThread_nolock();
        }
        cv_.notify_one();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> g(m_);
            stop_ = true;
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
    }

    void restart() { // apres stop() pour un nouveau cycle
        std::lock_guard<std::mutex> g(m_);
        stop_ = false;
        ensureThread_nolock();
    }

private:
    void ensureThread_nolock() {
        if (!th_.joinable()) th_ = std::thread([this] { run(); });
    }

    void run() {
        std::unique_lock<std::mutex> lk(m_);
        while (!stop_) {
            cv_.wait(lk, [this] { return pending_ || stop_; });
            if (stop_) break;
            pending_ = false;
            auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
            for (;;) {
                if (stop_) return;
                if (pending_) {
                    pending_ = false;
                    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                }
                if (std::chrono::steady_clock::now() >= deadline) break;
                cv_.wait_until(lk, deadline);
            }
            if (stop_) break;
            lk.unlock();
            DataStore::saveNow(); // ecriture reelle (dirty gere en interne)
            lk.lock();
        }
    }

    std::mutex m_;
    std::condition_variable cv_;
    std::thread th_;
    bool pending_ = false;
    bool stop_ = false;
};

// Heap singleton : pas de problemes d'ordre de destruction statique
Debouncer& debouncer() {
    static Debouncer* d = new Debouncer();
    return *d;
}

std::mutex& saveMutex() {
    static std::mutex m;
    return m;
}

bool& saveDirty() {
    static bool dirty = false;
    return dirty;
}

} // namespace

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

bool DataStore::isPortable = false;

fs::path DataStore::dir() {
    if (const char* td = std::getenv("TL_DATA_DIR"); td && *td)
        return fs::path(td);
    if (isPortable)
        return exeDir() / "data";
    // Phase A : racine de contenu configurable (persistante, comme TL_DATA_DIR).
    if (!settings.contentPath.empty()) return fs::path(settings.contentPath);
#ifdef _WIN32
    if (const char* la = std::getenv("LOCALAPPDATA"); la && *la)
        return fs::path(la) / "TeamLauncher";
#else
    // XDG Base Directory : $XDG_DATA_HOME sinon ~/.local/share.
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return fs::path(xdg) / "TeamLauncher";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".local" / "share" / "TeamLauncher";
#endif
    return exeDir() / "data";
}

fs::path DataStore::instancesRoot() { return fs::path(settings.instancesDir); }
fs::path DataStore::skinsDir() { return dir() / "skins"; }
fs::path DataStore::imagesDir() { return dir() / "images"; }
fs::path DataStore::configPath() { return dir() / "config.json"; }

// ---------------------------------------------------------------------------
// Defaults (default.env — fichier externe, jamais embarque)
// ---------------------------------------------------------------------------

std::map<std::string, std::string, CaseInsensitiveLess>& DataStore::defaults() {
    static std::map<std::string, std::string, CaseInsensitiveLess> d;
    return d;
}

void DataStore::loadDefaults() {
    auto& d = defaults();
    d.clear();

    const fs::path candidates[] = {
        exeDir() / "assets" / "default.env",
        exeDir() / "default.env",
        fs::current_path() / "assets" / "default.env",
    };
    for (const auto& p : candidates) {
        const std::string content = readFile(p);
        if (content.empty()) continue;
        std::istringstream ss(content);
        std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            if (line.empty() || line[0] == '#' || line.rfind("//", 0) == 0) continue;
            const size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            d[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
        }
        if (!d.empty()) break;
    }

    // Fallback dur si le fichier est absent (identique au C#). Le webhook
    // Discord n'y figure plus en clair (S3) : il est obfusque dans le binaire
    // et injecte juste apres, sur TOUS les chemins (cf. ci-dessous).
    if (d.empty()) {
        d["DISCORD_APP_ID"] = "1541468112740941844";
        d["DISCORD_ENABLED"] = "true";
        d["TELEMETRY_ENABLED"] = "true";
        d["UPDATE_URL"] =
            "https://raw.githubusercontent.com/teamstarwars-dev/Team-Luncher-/main/version.json";
        d["LANGUAGE"] = "fr";
        d["FPS_COUNTER_ENABLED"] = "false";
    }

    // Webhook Discord (S3) : retire de default.env (jamais en clair sur
    // disque) et obfusque ici. Les deux cas sont couverts — fichier absent
    // (bloc ci-dessus) ou fichier present sans la ligne — pour conserver le
    // comportement « webhook non vide par defaut » d'applyDefaults().
    if (d.find("DISCORD_TELEMETRY_WEBHOOK") == d.end())
        d["DISCORD_TELEMETRY_WEBHOOK"] = TL_OBF(
            "https://discord.com/api/webhooks/1412511652776083586/"
            "DnZ5eAZW5KwQCZJ0Cxy9e9m6AyLXWiNC-6JO6fIwS4yIkV-fLqZB6z-3c9x6s4CmhE-_");
}

void DataStore::applyDefaults(bool applyBooleans) {
    const auto& d = defaults();
    auto get = [&d](const char* k) -> const std::string* {
        auto it = d.find(k);
        return it == d.end() ? nullptr : &it->second;
    };

    if (settings.discordAppId.empty())
        if (const auto* v = get("DISCORD_APP_ID")) settings.discordAppId = *v;

    if (applyBooleans) {
        if (!settings.discordEnabled)
            if (const auto* v = get("DISCORD_ENABLED")) settings.discordEnabled = equalsIgnoreCase(*v, "true");
        if (!settings.telemetryEnabled)
            if (const auto* v = get("TELEMETRY_ENABLED")) settings.telemetryEnabled = equalsIgnoreCase(*v, "true");
    }

    if (settings.discordTelemetryWebhook.empty())
        if (const auto* v = get("DISCORD_TELEMETRY_WEBHOOK")) settings.discordTelemetryWebhook = *v;
    if (settings.updateUrl.empty())
        if (const auto* v = get("UPDATE_URL")) settings.updateUrl = *v;
    if (settings.language.empty())
        if (const auto* v = get("LANGUAGE")) settings.language = *v;
    if (settings.curseForgeApiKey.empty())
        if (const auto* v = get("CURSEFORGE_API_KEY")) settings.curseForgeApiKey = *v;
    if (!settings.fpsCounterEnabled)
        if (const auto* v = get("FPS_COUNTER_ENABLED")) settings.fpsCounterEnabled = equalsIgnoreCase(*v, "true");
}

// ---------------------------------------------------------------------------
// JSON (cles PascalCase, compat System.Text.Json de la v5)
// ---------------------------------------------------------------------------

// Champs sensibles ecrits chiffres ("enc:v1:" + DPAPI) dans config.json.
// En memoire ils restent en clair (l'UI et les modules les consomment tels quels).

// PteroHosts[] : seule la cle API par hote est un secret (ApiKey, cf. server_host).
static json encPteroHosts(const json& hosts) {
    json out = hosts;
    if (!out.is_array()) return out;
    for (auto& h : out) {
        if (!h.is_object()) continue;
        auto it = h.find("ApiKey");
        if (it != h.end() && it->is_string())
            *it = secrets::encrypt_value(it->get<std::string>());
    }
    return out;
}

static void decPteroHosts(json& hosts, bool* plainSecrets) {
    if (!hosts.is_array()) return;
    for (auto& h : hosts) {
        if (!h.is_object()) continue;
        auto it = h.find("ApiKey");
        if (it == h.end() || !it->is_string()) continue;
        std::string v = it->get<std::string>();
        if (!v.empty() && !secrets::is_encrypted(v) && plainSecrets)
            *plainSecrets = true; // ancien fichier v5 en clair -> rechiffre au save
        *it = secrets::decrypt_value(v);
    }
}

static json serialize(const AppSettings& s) {
    return json{
        {"PlayerName", s.playerName},
        {"AccountMode", s.accountMode},
        {"JavaPath", s.javaPath},
        {"MaxRamGb", s.maxRamGb},
        {"AzureClientId", secrets::encrypt_value(s.azureClientId)},
        {"InstancesDir", s.instancesDir},
        {"BgColor", s.bgColor},
        {"CardColor", s.cardColor},
        {"AccentColor", s.accentColor},
        {"BackgroundImagePath", s.backgroundImagePath},
        {"FpsCounterEnabled", s.fpsCounterEnabled},
        {"DiscordEnabled", s.discordEnabled},
        {"DiscordAppId", s.discordAppId},
        {"UpdateUrl", s.updateUrl},
        {"NewsUrl", s.newsUrl},
        {"Language", s.language},
        {"CurseForgeApiKey", secrets::encrypt_value(s.curseForgeApiKey)},
        {"OnboardingDone", s.onboardingDone},
        {"Instances", s.instances},
        {"Servers", s.servers},
        {"FavoriteServers", s.favoriteServers},
        {"Cities", s.cities},
        {"HostedServers", s.hostedServers},
        {"PteroHosts", encPteroHosts(s.pteroHosts)},
        {"AutoShortcut", s.autoShortcut},
        {"VpsUrl", s.vpsUrl},
        {"VpsApiKey", secrets::encrypt_value(s.vpsApiKey)},
        {"TelemetryEnabled", s.telemetryEnabled},
        {"DiscordTelemetryWebhook", secrets::encrypt_value(s.discordTelemetryWebhook)},
        {"InstallationId", s.installationId},
        {"AdminTelemetryEnabled", s.adminTelemetryEnabled},
        {"AdminServerUrl", secrets::encrypt_value(s.adminServerUrl)},
        {"MinimizeOnLaunch", s.minimizeOnLaunch},
        {"LogLevel", s.logLevel},
        {"Theme", s.theme},
        {"UiFont", s.uiFont},
        {"SidebarCompact", s.sidebarCompact},
        {"BackupAutoHours", s.backupAutoHours},
        {"BackupKeep", s.backupKeep},
        {"ColorblindMode", s.colorblind},
        {"FontScale", s.fontScale},
        {"ContentPath", s.contentPath},
        {"UpdateCheckHours", s.updateFreqHours},
        {"UpdateChannel", s.updateChannel},
        {"MaxDownloads", s.maxDownloads},
        {"BackupSpaceMb", s.backupSpaceMb},
        {"AnalyseThreads", s.analyseThreads},
        {"CloseBehavior", s.closeBehavior},
        {"LaunchAtSystemStart", s.launchAtSystemStart},
        {"StartupGame", s.startupGame},
        {"LastGameId", s.lastGameId},
        {"DateFormat", s.dateFormat},
        {"LastRunVersion", s.lastRunVersion},
        {"LastUpdateCheckUnix", s.lastUpdateCheckUnix},
        {"HelpDiscordUrl", s.helpDiscordUrl},
    };
}

// Lance une exception si une cle connue a un type invalide (comme System.Text.Json).
// plainSecrets (optionnel) signale qu'une valeur sensible etait lue en clair :
// le fichier est alors rechiffre au prochain enregistrement (migration v5 -> v6).
static void mergeInto(AppSettings& s, const json& j, bool* plainSecrets = nullptr) {
    if (!j.is_object()) throw std::runtime_error("config: objet attendu");

    auto getS = [&j](const char* k, std::string& out) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return;
        if (!it->is_string()) throw std::runtime_error("config: string attendue");
        out = it->get<std::string>();
    };
    // Lecture d'un champ sensible : detecte l'ancien format clair puis dechiffre.
    auto getSec = [&, plainSecrets](const char* k, std::string& out) {
        getS(k, out);
        if (out.empty()) return;
        if (!secrets::is_encrypted(out) && plainSecrets) *plainSecrets = true;
        out = secrets::decrypt_value(out);
    };
    auto getB = [&j](const char* k, bool& out) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return;
        if (!it->is_boolean()) throw std::runtime_error("config: bool attendu");
        out = it->get<bool>();
    };
    auto getI = [&j](const char* k, int& out) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return;
        if (!it->is_number()) throw std::runtime_error("config: nombre attendu");
        out = it->get<int>();
    };
    auto getI64 = [&j](const char* k, long long& out) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return;
        if (!it->is_number()) throw std::runtime_error("config: nombre attendu");
        out = it->get<long long>();
    };
    auto getD = [&j](const char* k, double& out) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return;
        if (!it->is_number()) throw std::runtime_error("config: nombre attendu");
        out = it->get<double>();
    };
    auto getA = [&j](const char* k, json& out) {
        auto it = j.find(k);
        if (it == j.end() || it->is_null()) return;
        if (!it->is_array()) throw std::runtime_error("config: array attendu");
        out = *it;
    };

    getS("PlayerName", s.playerName);
    getS("AccountMode", s.accountMode);
    getS("JavaPath", s.javaPath);
    getI("MaxRamGb", s.maxRamGb);
    getSec("AzureClientId", s.azureClientId);
    getS("InstancesDir", s.instancesDir);
    getS("BgColor", s.bgColor);
    getS("CardColor", s.cardColor);
    getS("AccentColor", s.accentColor);
    getS("BackgroundImagePath", s.backgroundImagePath);
    getB("FpsCounterEnabled", s.fpsCounterEnabled);
    getB("DiscordEnabled", s.discordEnabled);
    getS("DiscordAppId", s.discordAppId);
    getS("UpdateUrl", s.updateUrl);
    getS("NewsUrl", s.newsUrl);
    getS("Language", s.language);
    getSec("CurseForgeApiKey", s.curseForgeApiKey);
    getB("OnboardingDone", s.onboardingDone);
    getA("Instances", s.instances);
    getA("Servers", s.servers);
    {
        auto it = j.find("FavoriteServers");
        if (it != j.end() && !it->is_null()) {
            if (!it->is_array()) throw std::runtime_error("config: array attendu");
            s.favoriteServers = it->get<std::vector<std::string>>();
        }
    }
    getA("Cities", s.cities);
    getA("HostedServers", s.hostedServers);
    getA("PteroHosts", s.pteroHosts);
    decPteroHosts(s.pteroHosts, plainSecrets);
    getB("AutoShortcut", s.autoShortcut);
    getS("VpsUrl", s.vpsUrl);
    getSec("VpsApiKey", s.vpsApiKey);
    getB("TelemetryEnabled", s.telemetryEnabled);
    getSec("DiscordTelemetryWebhook", s.discordTelemetryWebhook);
    getS("InstallationId", s.installationId);
    getB("AdminTelemetryEnabled", s.adminTelemetryEnabled);
    getSec("AdminServerUrl", s.adminServerUrl);
    getB("MinimizeOnLaunch", s.minimizeOnLaunch);
    getS("LogLevel", s.logLevel);
    getS("Theme", s.theme);
    getS("UiFont", s.uiFont);
    getB("SidebarCompact", s.sidebarCompact);
    getI("BackupAutoHours", s.backupAutoHours);
    getI("BackupKeep", s.backupKeep);
    getB("ColorblindMode", s.colorblind);
    getD("FontScale", s.fontScale);
    getS("ContentPath", s.contentPath);
    getI("UpdateCheckHours", s.updateFreqHours);
    getS("UpdateChannel", s.updateChannel);
    getI("MaxDownloads", s.maxDownloads);
    getI("BackupSpaceMb", s.backupSpaceMb);
    getI("AnalyseThreads", s.analyseThreads);
    getS("CloseBehavior", s.closeBehavior);
    getB("LaunchAtSystemStart", s.launchAtSystemStart);
    getS("StartupGame", s.startupGame);
    getS("LastGameId", s.lastGameId);
    getS("DateFormat", s.dateFormat);
    getS("LastRunVersion", s.lastRunVersion);
    getI64("LastUpdateCheckUnix", s.lastUpdateCheckUnix);
    getS("HelpDiscordUrl", s.helpDiscordUrl);
}

// ---------------------------------------------------------------------------
// Load / Save
// ---------------------------------------------------------------------------

void DataStore::load() {
    loadDefaults();

    if (settings.installationId.empty())
        settings.installationId = newInstallationId();

    // Garde-fou InstancesDir (identique au C#, avant lecture du fichier)
    if (settings.instancesDir.empty() ||
        settings.instancesDir.find("TeamLauncher") == std::string::npos)
        settings.instancesDir = (dir() / "instances").string();

    try {
        const bool configExisted = fs::exists(configPath());
        bool plainSecrets = false; // secrets lus en clair -> a rechiffrer
        if (configExisted) {
            const json parsed = json::parse(readFile(configPath()));
            if (!parsed.is_null()) {
                AppSettings candidate = settings;
                mergeInto(candidate, parsed, &plainSecrets); // peut throw -> settings inchange
                settings = std::move(candidate);
                if (settings.instancesDir.empty() ||
                    settings.instancesDir.find("TeamLauncher") == std::string::npos)
                    settings.instancesDir = (dir() / "instances").string();
            }
        }

        const bool firstRealConfig = !configExisted || settings.discordAppId.empty();
        const bool wasEmpty = settings.updateUrl.empty() ||
                              settings.discordTelemetryWebhook.empty();
        applyDefaults(firstRealConfig);

        // Ecrire immediatement si on a injecte des defauts OU migre d'anciens
        // secrets en clair (fichiers v5) vers le format "enc:v1:".
        if (configExisted && (plainSecrets ||
                              (wasEmpty && (!settings.updateUrl.empty() ||
                                            !settings.discordTelemetryWebhook.empty()))))
            doSave();
    } catch (...) {
        // config corrompue : on garde les valeurs par defaut
    }

    std::error_code ec;
    fs::create_directories(settings.instancesDir, ec);
    fs::create_directories(skinsDir(), ec);
    fs::create_directories(imagesDir(), ec);
}

void DataStore::save() {
    saveDirty() = true;
    debouncer().request();
}

void DataStore::saveNow() {
    saveDirty() = false;
    doSave();
}

void DataStore::doSave() {
    std::lock_guard<std::mutex> g(saveMutex());
    std::error_code ec;
    fs::create_directories(dir(), ec);

    const std::string text = serialize(settings).dump(4);
    const fs::path target = configPath();
    fs::path tmp = target;
    tmp += ".tmp";

    for (int attempt = 1; attempt <= 6; ++attempt) {
        bool written = false;
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (out) {
                out.write(text.data(), static_cast<std::streamsize>(text.size()));
                out.flush();
                written = !out.fail();
            }
        } // le flux DOIT etre ferme avant le remplacement (sinon err 32 sharing violation sous Windows)
#ifdef _WIN32
        if (written && MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
            return;
#else
        // rename() POSIX : remplacement atomique sur le meme fs (tmp est
        // frere de target, donc meme fs sauf montage exotique).
        if (written && std::rename(tmp.string().c_str(), target.string().c_str()) == 0)
            return;
#endif
        fs::remove(tmp, ec);
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
    }

    // Dernier recours : ecriture directe
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

void DataStore::shutdown() {
    debouncer().stop();
}

AppSettings DataStore::settings{};

// ---------------------------------------------------------------------------
// Instances (partage entre l'UI et les importeurs de modpacks)
// ---------------------------------------------------------------------------

std::string new_guid() {
    unsigned char b[16];
#ifdef _WIN32
    BCryptGenRandom(nullptr, b, sizeof(b), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
#else
    // getrandom() : pas de fd a gerer, jamais bloquant passe l'init.
    size_t done = 0;
    while (done < sizeof(b)) {
        const ssize_t n = ::getrandom(b + done, sizeof(b) - done, 0);
        if (n <= 0) break;
        done += static_cast<size_t>(n);
    }
    if (done < sizeof(b)) { // repli : ne jamais renvoyer un GUID partiellement nul
        std::random_device rd;
        for (; done < sizeof(b); ++done) b[done] = static_cast<unsigned char>(rd());
    }
#endif
    char out[33];
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", b[i]);
    out[32] = '\0';
    return out;
}

json make_instance(const std::string& name, const std::string& loader,
                   const std::string& mcVersion) {
    json inst;
    inst["Id"] = new_guid();
    inst["Name"] = name;
    inst["Description"] = "";
    inst["ImagePath"] = "";
    inst["Loader"] = loader;
    inst["McVersion"] = mcVersion;
    inst["Launches"] = 0;
    inst["PlaySeconds"] = 0;
    inst["MaxRamGb"] = 0;
    inst["JvmArgs"] = "";
    inst["Notes"] = "";
    inst["LastPlayed"] = "0001-01-01T00:00:00";
    return inst;
}

} // namespace tl

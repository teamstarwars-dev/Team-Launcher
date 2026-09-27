#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "obf.hpp" // adminServerUrl obfusquee (S3, barriere `strings`)

namespace tl {

struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const noexcept;
};

struct AppSettings {
    std::string playerName = "Joueur";
    std::string accountMode;
    std::string javaPath;
    int maxRamGb = 4;
    std::string azureClientId;
    std::string instancesDir;
    std::string bgColor;
    std::string cardColor;
    std::string accentColor;
    std::string backgroundImagePath;
    bool fpsCounterEnabled = false;
    bool discordEnabled = false;
    std::string discordAppId;
    std::string updateUrl;
    std::string newsUrl;
    std::string language = "fr";
    std::string curseForgeApiKey;
    bool onboardingDone = false;
    // Modeles non encore portes : conservees telles quelles (compat config C#)
    nlohmann::json instances = nlohmann::json::array();
    nlohmann::json servers = nlohmann::json::array();
    std::vector<std::string> favoriteServers;
    nlohmann::json cities = nlohmann::json::array();
    nlohmann::json hostedServers = nlohmann::json::array();
    nlohmann::json pteroHosts = nlohmann::json::array(); // hotes Pterodactyl (server_host)
    bool autoShortcut = false;
    std::string vpsUrl;
    std::string vpsApiKey;
    bool telemetryEnabled = true;
    std::string discordTelemetryWebhook;
    std::string installationId;
    bool adminTelemetryEnabled = false;
    // IP du serveur d'admin : obfusquee (S3) pour ne pas la laisser en clair
    // dans .rdata (elle est de toute facon chiffree DPAPI dans config.json).
    std::string adminServerUrl = TL_OBF("http://51.255.207.183:3000");
    bool minimizeOnLaunch = true;
};

// Identifiant d'instance : 32 hexa (Guid .NET « N »), BCryptGenRandom.
std::string new_guid();

// InstanceInfo C# (champs minimaux — les autres prennent les defauts C#).
nlohmann::json make_instance(const std::string& name, const std::string& loader,
                             const std::string& mcVersion);

// Portage de DataStore.cs (C#) : config.json partage avec la v5 Avalonia.
class DataStore {
public:
    static bool isPortable;

    // TL_DATA_DIR (test) > --portable (exe/data) > %LOCALAPPDATA%\TeamLauncher
    static std::filesystem::path dir();
    static std::filesystem::path instancesRoot();
    static std::filesystem::path skinsDir();
    static std::filesystem::path imagesDir();
    static std::filesystem::path configPath();

    static void load();
    static void save();    // ecriture deboncee 500 ms
    static void saveNow(); // ecriture immediate (fin d'app)
    static void shutdown(); // arrete le worker debonce

    static AppSettings settings;

private:
    static void loadDefaults();
    static void applyDefaults(bool applyBooleans);
    static void doSave();

    static std::map<std::string, std::string, CaseInsensitiveLess>& defaults();
};

} // namespace tl

#include "datastore.hpp"

#include "test_env.hpp" // _putenv_s portable (Windows/POSIX)

#include "secrets.hpp" // marqueur de chiffrement des champs sensibles
#include "util_str.hpp" // conversions UTF (section POSIX)

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>

namespace fs = std::filesystem;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static std::string readText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-datastore-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#else
    setenv("TL_DATA_DIR", tmp.string().c_str(), 1);
#endif

    using tl::DataStore;

    // --- 1. premier load sans config ---
    DataStore::settings = tl::AppSettings{};
    DataStore::load();
    CHECK(DataStore::settings.playerName == "Joueur");
    CHECK(DataStore::settings.language == "fr");
    // Vide = API GitHub Releases, qui choisit l'archive de la bonne
    // plateforme. Une URL unique vers un version.json ne le sait pas :
    // elle servirait un .exe a un utilisateur Linux.
    CHECK(DataStore::settings.updateUrl.empty());
    CHECK(!DataStore::settings.discordAppId.empty()); // temoin : defauts appliques
    CHECK(DataStore::settings.discordEnabled);         // defaut bool applique (1er config)
    CHECK(DataStore::settings.instancesDir == (DataStore::dir() / "instances").string());
    CHECK(fs::exists(tmp / "instances"));
    CHECK(fs::exists(tmp / "skins"));
    CHECK(fs::exists(tmp / "images"));
    CHECK(!fs::exists(tmp / "config.json"));           // pas d'ecriture auto au 1er load
    CHECK(!DataStore::settings.installationId.empty());

    // --- 2. mutation + saveNow ---
    DataStore::settings.playerName = "Steve";
    DataStore::settings.maxRamGb = 8;
    DataStore::settings.favoriteServers = {"mc.example.org", "autre.serveur.net"};
    DataStore::settings.onboardingDone = true;
    DataStore::saveNow();
    const fs::path cfg = tmp / "config.json";
    CHECK(fs::exists(cfg));
    const std::string text = readText(cfg);
    CHECK(text.find("\"PlayerName\"") != std::string::npos); // cles PascalCase
    CHECK(text.find("\"Steve\"") != std::string::npos);
    CHECK(text.find('\n') != std::string::npos);             // WriteIndented

    // --- 3. re-load restaure les valeurs ---
    DataStore::settings = tl::AppSettings{};
    DataStore::load();
    CHECK(DataStore::settings.playerName == "Steve");
    CHECK(DataStore::settings.maxRamGb == 8);
    CHECK(DataStore::settings.favoriteServers.size() == 2);
    CHECK(DataStore::settings.favoriteServers[0] == "mc.example.org");
    CHECK(DataStore::settings.onboardingDone);
    // applyDefaults ne remplit QUE ce que les defauts fournissent : sans
    // UPDATE_URL, le reglage reste vide, et c'est la valeur voulue.
    CHECK(DataStore::settings.updateUrl.empty());
    CHECK(!DataStore::settings.discordAppId.empty()); // temoin : defauts appliques

    // --- 4. save() debonce 500 ms ---
    DataStore::settings.playerName = "Debounced";
    DataStore::save();
    std::this_thread::sleep_for(std::chrono::milliseconds(1300));
    CHECK(readText(cfg).find("Debounced") != std::string::npos);

    // --- 5. config corrompu : pas de crash, valeurs initiales conservees ---
    {
        std::ofstream o(cfg, std::ios::binary | std::ios::trunc);
        o << "{ not json !!!";
    }
    DataStore::settings = tl::AppSettings{};
    DataStore::load();
    CHECK(DataStore::settings.playerName == "Joueur"); // initializer garde
    CHECK(DataStore::settings.language == "fr");
    CHECK(DataStore::settings.updateUrl.empty());      // applyDefaults saute (comme le C#)
    CHECK(DataStore::settings.instancesDir == (DataStore::dir() / "instances").string());
    CHECK(fs::exists(tmp / "instances"));              // dirs crees malgre tout

    // --- 6. secrets : chiffres sur disque (enc:v1: + DPAPI), recharges en clair ---
    {
        std::ofstream o(cfg, std::ios::binary | std::ios::trunc);
        o << "{}";
    }
    DataStore::settings = tl::AppSettings{};
    DataStore::load();
    DataStore::settings.curseForgeApiKey = "$2a$10$secretcurseforgekey";
    DataStore::settings.vpsApiKey = "vps-secret-123";
    DataStore::settings.discordTelemetryWebhook =
        "https://discord.com/api/webhooks/1/SECRET";
    DataStore::settings.pteroHosts = nlohmann::json::array(
        {nlohmann::json{{"Name", "h1"}, {"ApiKey", "ptero-secret"}}});
    DataStore::saveNow();
    const std::string secText = readText(cfg);
    CHECK(secText.find("$2a$10$secretcurseforgekey") == std::string::npos); // jamais en clair
    CHECK(secText.find("vps-secret-123") == std::string::npos);
    CHECK(secText.find("/webhooks/1/SECRET") == std::string::npos);        // webhook masque
    CHECK(secText.find("ptero-secret") == std::string::npos);
    CHECK(secText.find(tl::secrets::kEncPrefix) != std::string::npos);     // marqueur enc:v1:
    DataStore::settings = tl::AppSettings{};
    DataStore::load();
    CHECK(DataStore::settings.curseForgeApiKey == "$2a$10$secretcurseforgekey");
    CHECK(DataStore::settings.vpsApiKey == "vps-secret-123");
    CHECK(DataStore::settings.discordTelemetryWebhook ==
          "https://discord.com/api/webhooks/1/SECRET");
    CHECK(DataStore::settings.pteroHosts[0]["ApiKey"] == "ptero-secret");

    // --- 7. compat v5 : ancien fichier en clair -> valeur conservee + rechiffre ---
    {
        std::ofstream o(cfg, std::ios::binary | std::ios::trunc);
        o << R"({"PlayerName":"Ancien","CurseForgeApiKey":"clair-v5",)"
             R"("VpsApiKey":"vps-clair","PteroHosts":[{"Name":"h","ApiKey":"pan-clair"}]})";
    }
    DataStore::settings = tl::AppSettings{};
    DataStore::load();
    CHECK(DataStore::settings.playerName == "Ancien");
    CHECK(DataStore::settings.curseForgeApiKey == "clair-v5"); // lecture identique
    CHECK(DataStore::settings.vpsApiKey == "vps-clair");
    CHECK(DataStore::settings.pteroHosts[0]["ApiKey"] == "pan-clair");
    const std::string migrated = readText(cfg);
    CHECK(migrated.find("clair-v5") == std::string::npos);             // rechiffre au load
    CHECK(migrated.find("pan-clair") == std::string::npos);
    CHECK(migrated.find(tl::secrets::kEncPrefix) != std::string::npos);
    DataStore::settings = tl::AppSettings{};
    DataStore::load(); // le fichier migre se relit correctement
    CHECK(DataStore::settings.curseForgeApiKey == "clair-v5");
    CHECK(DataStore::settings.pteroHosts[0]["ApiKey"] == "pan-clair");

#ifndef _WIN32
    // --- 8. POSIX : XDG, getrandom, conversions UTF (Linux uniquement) ---
    {
        unsetenv("TL_DATA_DIR");
        const fs::path xdg = tmp / "xdg-home";
        fs::create_directories(xdg);
        setenv("XDG_DATA_HOME", xdg.string().c_str(), 1);
        CHECK(DataStore::dir() == xdg / "TeamLauncher");
        unsetenv("XDG_DATA_HOME");
        if (const char* home = std::getenv("HOME"); home && *home)
            CHECK(DataStore::dir() == fs::path(home) / ".local" / "share" / "TeamLauncher");
        // GUID : 32 hexa (getrandom, jamais partiellement nul)
        const std::string g = tl::new_guid();
        CHECK(g.size() == 32);
        bool hex = true;
        for (char c : g)
            hex = hex && (std::isxdigit(static_cast<unsigned char>(c)) != 0);
        CHECK(hex);
        CHECK(g != std::string(32, '0'));
        // UTF-8 <-> UTF-32 : round-trip, y compris hors BMP
        const std::string u8 = "h\xC3\xA9llo \xF0\x9F\x8C\x8D"; // héllo + globe
        const std::wstring w = tl::utf8_to_wide(u8);
        CHECK(w.size() == 7); // 5 lettres + e-accent + 1 scalaire astral
        CHECK(tl::wide_to_utf8(w.c_str()) == u8);
        CHECK(tl::utf8_to_wide({}).empty());
        CHECK(tl::wide_to_utf8(nullptr).empty());
        CHECK(tl::wide_to_utf8(L"").empty());
        // restaure l'env des sections suivantes / du shutdown
        setenv("TL_DATA_DIR", tmp.string().c_str(), 1);
    }
#endif

    // --- Migration de MinimizeOnLaunch vers OnGameLaunch ---------------
    // Une configuration ecrite avant l'ajout du reglage n'a que le
    // booleen. En deriver la valeur evite qu'un utilisateur ayant decoche
    // « minimiser » voie le launcher se minimiser de nouveau.
    {
        const auto cfg = DataStore::configPath();
        auto reload = [&](const char* json) {
            { std::ofstream o(cfg, std::ios::trunc); o << json; }
            DataStore::settings = tl::AppSettings{};
            DataStore::load();
        };

        reload(R"({"MinimizeOnLaunch":false})");
        CHECK(DataStore::settings.onGameLaunch == "nothing");
        CHECK(!DataStore::settings.minimizeOnLaunch);

        reload(R"({"MinimizeOnLaunch":true})");
        CHECK(DataStore::settings.onGameLaunch == "minimize");

        // Le nouveau reglage l'emporte sur l'ancien booleen.
        reload(R"({"MinimizeOnLaunch":true,"OnGameLaunch":"quit"})");
        CHECK(DataStore::settings.onGameLaunch == "quit");
        // Et le miroir ecrit pour la v5 Avalonia suit : « quitter » n'a
        // pas d'equivalent chez elle, on l'ecrit « ne pas minimiser ».
        CHECK(!DataStore::settings.minimizeOnLaunch);

        // Valeur inconnue : on retombe sur le defaut plutot que de la
        // garder, sinon le launcher ne ferait rien du tout en silence.
        reload(R"({"OnGameLaunch":"n'importe quoi"})");
        CHECK(DataStore::settings.onGameLaunch == "minimize");

        // Aller-retour : ce qui est ecrit doit se relire a l'identique.
        DataStore::settings.onGameLaunch = "quit";
        DataStore::saveNow();
        DataStore::settings = tl::AppSettings{};
        DataStore::load();
        CHECK(DataStore::settings.onGameLaunch == "quit");
    }

    DataStore::shutdown();
    fs::remove_all(tmp, ec);

    if (g_failures) {
        std::printf("TESTS FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("ALL TESTS PASSED\n");
    return 0;
}

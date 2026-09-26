#include "datastore.hpp"

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
    CHECK(!DataStore::settings.updateUrl.empty());     // default.env ou fallback
    CHECK(!DataStore::settings.discordAppId.empty());
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
    CHECK(!DataStore::settings.updateUrl.empty()); // applyDefaults remplit les vides

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

    DataStore::shutdown();
    fs::remove_all(tmp, ec);

    if (g_failures) {
        std::printf("TESTS FAILED: %d\n", g_failures);
        return 1;
    }
    std::printf("ALL TESTS PASSED\n");
    return 0;
}

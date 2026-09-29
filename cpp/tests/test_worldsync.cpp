// Tests de la synchronisation des mondes : detection des emplacements
// CurseForge / launcher, rapprochement par nom d'instance, et import avec
// sauvegarde prealable du monde ecrase.

#include "worldsync.hpp"

#include "test_env.hpp" // _putenv_s portable (Windows/POSIX)

#include "datastore.hpp"
#include "miniz.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace tl;
using nlohmann::json;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

struct Buf {
    std::vector<std::uint8_t> v;
    void u8(int b) { v.push_back(static_cast<std::uint8_t>(b)); }
    void i16(int x) { u8((x >> 8) & 0xFF); u8(x & 0xFF); }
    void i32(long long x) { for (int s = 24; s >= 0; s -= 8) u8((x >> s) & 0xFF); }
    void i64(long long x) { for (int s = 56; s >= 0; s -= 8) u8((x >> s) & 0xFF); }
    void str(const std::string& s) {
        i16(static_cast<int>(s.size()));
        for (char c : s) u8(static_cast<unsigned char>(c));
    }
    void tag(int t, const std::string& n) { u8(t); str(n); }
};

static void write_level(const fs::path& worldDir, const std::string& name,
                        long long lastPlayed) {
    std::error_code ec;
    fs::create_directories(worldDir, ec);
    Buf b;
    b.u8(10); b.str("");
    b.tag(10, "Data");
    b.tag(8, "LevelName"); b.str(name);
    b.tag(4, "LastPlayed"); b.i64(lastPlayed);
    b.u8(0);
    b.u8(0);
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(b.v.size()));
    std::vector<std::uint8_t> z(bound);
    mz_compress(z.data(), &bound, b.v.data(), static_cast<mz_ulong>(b.v.size()));
    z.resize(bound);
    std::vector<std::uint8_t> gz = {0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0xFF};
    gz.insert(gz.end(), z.begin() + 2, z.end() - 4);
    const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, b.v.data(), b.v.size());
    for (int s = 0; s < 32; s += 8) gz.push_back((crc >> s) & 0xFF);
    const std::uint32_t n = static_cast<std::uint32_t>(b.v.size());
    for (int s = 0; s < 32; s += 8) gz.push_back((n >> s) & 0xFF);
    std::ofstream o(worldDir / "level.dat", std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(gz.data()),
            static_cast<std::streamsize>(gz.size()));
}

static void write_text(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    o << s;
}

static std::string read_text(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-wsync-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    DataStore::load();

    // =====================================================================
    // 1. Les deux emplacements : minecraft/saves (CF) et saves (launcher)
    // =====================================================================
    {
        const fs::path cf = tmp / "cfinst";
        write_level(cf / "minecraft" / "saves" / "Monde1", "Monde Un", 111);
        write_text(cf / "minecraft" / "saves" / "Monde1" / "data.bin", "ABC");
        // Dossier sans level.dat : ce n'est pas un monde
        fs::create_directories(cf / "minecraft" / "saves" / "PasUnMonde", ec);

        auto ws = wsync::list_worlds_in(cf, wsync::Origin::CurseForge);
        CHECK_EQ(ws.size(), size_t{1});
        if (ws.size() == 1) {
            CHECK_EQ(ws[0].worldFolder, std::string("Monde1"));
            CHECK_EQ(ws[0].displayName, std::string("Monde Un"));
            CHECK_EQ(ws[0].levelLastPlayed, 111LL);
            CHECK(ws[0].sizeBytes > 0);
            CHECK(ws[0].lastModified > 0);
            CHECK(ws[0].origin == wsync::Origin::CurseForge);
        }

        // Emplacement « launcher »
        const fs::path ln = tmp / "lninst";
        write_level(ln / "saves" / "Monde1", "Monde Un", 111);
        auto ws2 = wsync::list_worlds_in(ln, wsync::Origin::Launcher);
        CHECK_EQ(ws2.size(), size_t{1});

        // Racine inexistante
        CHECK(wsync::list_worlds_in(tmp / "nexistepas",
                                    wsync::Origin::Launcher).empty());
    }

    // =====================================================================
    // 2. Import : copie complete + sauvegarde du monde ecrase
    // =====================================================================
    {
        // Instance cible dans le config
        json inst = make_instance("Cible", "Forge", "1.20.1");
        const std::string id = inst.value("Id", "");
        DataStore::settings.instances = json::array({inst});

        const fs::path src = tmp / "srcinst" / "minecraft" / "saves" / "AImporter";
        write_level(src, "A Importer", 999);
        write_text(src / "region" / "r.0.0.mca", "REGION-NEUVE");
        write_text(src / "sous" / "dossier" / "f.txt", "IMBRIQUE");

        auto worlds = wsync::list_worlds_in(tmp / "srcinst",
                                            wsync::Origin::CurseForge);
        CHECK_EQ(worlds.size(), size_t{1});
        if (worlds.size() == 1) {
            // Premier import : rien a ecraser, donc pas de sauvegarde
            auto r1 = wsync::import_world(worlds[0], id);
            CHECK(r1.ok);
            CHECK(r1.error.empty());
            CHECK(r1.backup.empty());
            const fs::path dst =
                DataStore::instancesRoot() / id / "saves" / "AImporter";
            CHECK(fs::is_directory(dst, ec));
            CHECK_EQ(read_text(dst / "region" / "r.0.0.mca"),
                     std::string("REGION-NEUVE"));
            // L'arborescence imbriquee suit
            CHECK_EQ(read_text(dst / "sous" / "dossier" / "f.txt"),
                     std::string("IMBRIQUE"));

            // Second import par-dessus : le monde existant doit etre archive
            write_text(dst / "region" / "r.0.0.mca", "ANCIENNE-VERSION");
            auto r2 = wsync::import_world(worlds[0], id);
            CHECK(r2.ok);
            CHECK(!r2.backup.empty());
            CHECK(fs::is_regular_file(r2.backup, ec));
            CHECK(fs::file_size(r2.backup, ec) > 0);
            // Le contenu neuf a bien remplace l'ancien
            CHECK_EQ(read_text(dst / "region" / "r.0.0.mca"),
                     std::string("REGION-NEUVE"));
        }

        // Cas d'echec
        wsync::Snapshot bad;
        bad.path = tmp / "nexistepas";
        bad.worldFolder = "X";
        auto rb = wsync::import_world(bad, id);
        CHECK(!rb.ok);
        CHECK(!rb.error.empty());

        if (!worlds.empty()) {
            auto rn = wsync::import_world(worlds[0], "");
            CHECK(!rn.ok); // pas d'instance de destination
        }
    }

    // =====================================================================
    // 3. Detection CurseForge : ne doit jamais planter, meme sans CF installe
    // =====================================================================
    {
        const auto cfs = wsync::detect_curseforge_instances();
        std::printf("INFO instances CurseForge detectees : %zu\n", cfs.size());
        for (const auto& [p, n] : cfs) {
            CHECK(!n.empty());
            CHECK(fs::is_directory(p, ec));
        }
        // compare_all et detect_newer ne doivent rien lever
        const auto cmp = wsync::compare_all();
        const auto newer = wsync::detect_newer_from_curseforge();
        std::printf("INFO rapprochements : %zu, mondes plus recents : %zu\n",
                    cmp.size(), newer.size());
        for (const auto& c : cmp) CHECK(!c.instanceName.empty());
    }

    _putenv_s("TL_DATA_DIR", "");
    DataStore::shutdown();
    fs::remove_all(tmp, ec);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

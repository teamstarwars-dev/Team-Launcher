// Tests de la lecture des mondes : level.dat analyse pour de vrai (et non
// par scan d'octets comme WorldTools.cs), listing trie, reperes de version,
// menage des regions vides.

#include "world.hpp"

#include "datastore.hpp"
#include "miniz.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tl;

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

static std::vector<std::uint8_t> gzip_of(const std::vector<std::uint8_t>& in) {
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(in.size()));
    std::vector<std::uint8_t> z(bound);
    if (mz_compress(z.data(), &bound, in.data(),
                    static_cast<mz_ulong>(in.size())) != MZ_OK)
        return {};
    z.resize(bound);
    std::vector<std::uint8_t> out = {0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0xFF};
    out.insert(out.end(), z.begin() + 2, z.end() - 4);
    const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, in.data(), in.size());
    for (int s = 0; s < 32; s += 8) out.push_back((crc >> s) & 0xFF);
    const std::uint32_t n = static_cast<std::uint32_t>(in.size());
    for (int s = 0; s < 32; s += 8) out.push_back((n >> s) & 0xFF);
    return out;
}

// level.dat : racine -> Data { LevelName, LastPlayed, DataVersion, ... }
static void write_level(const fs::path& worldDir, const std::string& name,
                        long long lastPlayed, long long dataVersion, int gameType,
                        bool hardcore, long long seed) {
    std::error_code ec;
    fs::create_directories(worldDir, ec);
    Buf b;
    b.u8(10); b.str("");
    b.tag(10, "Data");
    b.tag(8, "LevelName"); b.str(name);
    b.tag(4, "LastPlayed"); b.i64(lastPlayed);
    b.tag(3, "DataVersion"); b.i32(dataVersion);
    b.tag(3, "GameType"); b.i32(gameType);
    b.tag(1, "hardcore"); b.u8(hardcore ? 1 : 0);
    b.tag(4, "RandomSeed"); b.i64(seed);
    b.u8(0); // fin Data
    b.u8(0); // fin racine
    const auto gz = gzip_of(b.v);
    std::ofstream o(worldDir / "level.dat", std::ios::binary | std::ios::trunc);
    o.write(reinterpret_cast<const char*>(gz.data()),
            static_cast<std::streamsize>(gz.size()));
}

static void write_bytes(const fs::path& p, std::size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    const std::vector<char> z(n, 0);
    o.write(z.data(), static_cast<std::streamsize>(n));
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-world-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#endif
    DataStore::load();

    const fs::path inst = DataStore::instancesRoot() / "inst1";
    const fs::path saves = inst / "saves";

    // =====================================================================
    // 1. Lecture d'un level.dat
    // =====================================================================
    {
        const fs::path w = saves / "MonMonde";
        write_level(w, "Mon Beau Monde", 1700000000000LL, 3465, 1, true, 42);
        write_bytes(w / "region" / "r.0.0.mca", 9000);   // region pleine
        write_bytes(w / "region" / "r.1.0.mca", 8192);   // region vide
        write_bytes(w / "region" / "r.2.0.mca", 100);    // region tronquee

        auto info = world::read_level(w);
        CHECK(info.has_value());
        if (info) {
            CHECK_EQ(info->name, std::string("Mon Beau Monde"));
            CHECK_EQ(info->folder, std::string("MonMonde"));
            CHECK_EQ(info->lastPlayed, 1700000000000LL);
            CHECK_EQ(info->dataVersion, 3465LL);
            CHECK_EQ(info->gameType, 1);
            CHECK(info->hardcore);
            CHECK_EQ(info->seed, 42LL);
            CHECK(info->sizeBytes > 9000);
            CHECK_EQ(info->regionCount, 3); // les 3 .mca sont bien nommes
        }

        // Regions vides : <= 8192 octets
        CHECK_EQ(world::count_empty_regions(w), 2);
        CHECK_EQ(world::delete_empty_regions(w), 2);
        CHECK_EQ(world::count_empty_regions(w), 0);
        CHECK(fs::exists(w / "region" / "r.0.0.mca")); // la pleine reste
        // Rien a refaire
        CHECK_EQ(world::delete_empty_regions(w), 0);
    }

    // =====================================================================
    // 2. Cas d'echec : pas de level.dat, fichier illisible, Data absent
    // =====================================================================
    {
        CHECK(!world::read_level(saves / "nexistepas").has_value());
        const fs::path bad = saves / "Casse";
        fs::create_directories(bad, ec);
        std::ofstream(bad / "level.dat", std::ios::binary) << "pas du gzip";
        CHECK(!world::read_level(bad).has_value());

        // NBT valide mais sans compound « Data »
        const fs::path nodata = saves / "SansData";
        fs::create_directories(nodata, ec);
        Buf b;
        b.u8(10); b.str("");
        b.tag(8, "Autre"); b.str("x");
        b.u8(0);
        const auto gz = gzip_of(b.v);
        std::ofstream o(nodata / "level.dat", std::ios::binary);
        o.write(reinterpret_cast<const char*>(gz.data()),
                static_cast<std::streamsize>(gz.size()));
        o.close();
        CHECK(!world::read_level(nodata).has_value());

        CHECK_EQ(world::count_empty_regions(saves / "nexistepas"), 0);
        CHECK_EQ(world::delete_empty_regions(saves / "nexistepas"), 0);
        CHECK_EQ(world::dir_size(saves / "nexistepas"), 0LL);
    }

    // =====================================================================
    // 3. Listing : tri du plus recent au plus ancien, dossiers sans level.dat
    //    ignores (« Casse » et « SansData » en font partie)
    // =====================================================================
    {
        write_level(saves / "Ancien", "Ancien", 1000000000000LL, 1343, 0, false, 1);
        write_level(saves / "Recent", "Recent", 1800000000000LL, 3465, 0, false, 2);
        const auto list = world::list_worlds(inst);
        CHECK_EQ(list.size(), size_t{3}); // MonMonde, Ancien, Recent
        if (list.size() == 3) {
            CHECK_EQ(list[0].name, std::string("Recent"));   // 1.8e12
            CHECK_EQ(list[1].name, std::string("Mon Beau Monde")); // 1.7e12
            CHECK_EQ(list[2].name, std::string("Ancien"));   // 1.0e12
        }
        // Instance sans dossier saves
        CHECK(world::list_worlds(DataStore::instancesRoot() / "vide").empty());
    }

    // =====================================================================
    // 4. Reperes de version
    // =====================================================================
    CHECK_EQ(world::version_name(1343), std::string("1.12.2"));
    CHECK_EQ(world::version_name(3465), std::string("~1.20.1"));
    CHECK_EQ(world::version_name(3953), std::string("1.21"));
    CHECK_EQ(world::version_name(0), std::string(""));
    CHECK_EQ(world::version_name(-5), std::string(""));
    CHECK_EQ(world::version_name(100), std::string("<= 1.9"));

    // =====================================================================
    // 5. Monde reel de la machine, s'il y en a un
    // =====================================================================
    {
        _putenv_s("TL_DATA_DIR", "");
        DataStore::load(); // revient sur le dossier de donnees reel
        const char* home = std::getenv("LOCALAPPDATA");
        bool tested = false;
        if (home) {
            const fs::path root = fs::path(home) / "TeamLauncher" / "instances";
            std::error_code e2;
            if (fs::is_directory(root, e2)) {
                for (const auto& e : fs::directory_iterator(root, e2)) {
                    if (e2 || tested) break;
                    if (!e.is_directory(e2)) continue;
                    const auto ws = world::list_worlds(e.path());
                    if (ws.empty()) continue;
                    const auto& w = ws[0];
                    std::printf("INFO monde reel : \"%s\" %s  %s  %d region(s)\n",
                                w.name.c_str(),
                                world::version_name(w.dataVersion).c_str(),
                                w.folder.c_str(), w.regionCount);
                    CHECK(!w.name.empty());
                    CHECK(w.sizeBytes > 0);
                    tested = true;
                }
            }
        }
        if (!tested) std::printf("INFO aucun monde reel trouve (test saute)\n");
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

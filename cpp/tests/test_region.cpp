// Tests de la lecture des regions .mca : nommage, en-tete, depaquetage des
// palettes (1.13-1.15 tasse vs 1.16+ rembourre), sections a Y negatif,
// effacement de chunks, et lecture d'un monde reel si la machine en a un.

#include "region.hpp"

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

// --- fabrication de NBT gros-boutiste --------------------------------------
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

static std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t>& in) {
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(in.size()));
    std::vector<std::uint8_t> out(bound);
    if (mz_compress(out.data(), &bound, in.data(),
                    static_cast<mz_ulong>(in.size())) != MZ_OK)
        return {};
    out.resize(bound);
    return out;
}

// Ecrit une region ne contenant qu'un chunk, en (lx, lz), scheme donne.
static bool write_region(const fs::path& p, int lx, int lz,
                         const std::vector<std::uint8_t>& chunkNbt, int scheme) {
    std::vector<std::uint8_t> payload;
    if (scheme == 2) payload = zlib_compress(chunkNbt);
    else if (scheme == 3) payload = chunkNbt;
    else return false;
    if (payload.empty()) return false;

    std::vector<std::uint8_t> file(8192, 0);
    const int i = lz * 32 + lx;
    file[i * 4] = 0; file[i * 4 + 1] = 0; file[i * 4 + 2] = 2; // offset = secteur 2
    file[i * 4 + 3] = 1;                                       // 1 secteur
    for (int k = 0; k < 4; ++k) file[4096 + i * 4 + k] = (k == 3) ? 0x2A : 0;

    const std::uint32_t len = static_cast<std::uint32_t>(payload.size() + 1);
    std::vector<std::uint8_t> sec(4096, 0);
    sec[0] = (len >> 24) & 0xFF; sec[1] = (len >> 16) & 0xFF;
    sec[2] = (len >> 8) & 0xFF;  sec[3] = len & 0xFF;
    sec[4] = static_cast<std::uint8_t>(scheme);
    if (payload.size() + 5 > sec.size()) sec.resize(payload.size() + 5, 0);
    std::copy(payload.begin(), payload.end(), sec.begin() + 5);

    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(file.data()), 8192);
    out.write(reinterpret_cast<const char*>(sec.data()),
              static_cast<std::streamsize>(sec.size()));
    return static_cast<bool>(out);
}

// Chunk 1.18+ : sections[{ Y, block_states{ palette, data } }]
// paletteNames : 2 entrees ; tous les blocs prennent l'indice 1 sauf (0,0,0).
static std::vector<std::uint8_t> make_chunk_modern(int sectionY, long long dataVersion) {
    Buf b;
    b.u8(10); b.str("");
    b.tag(3, "DataVersion"); b.i32(dataVersion);
    b.tag(9, "sections"); b.u8(10); b.i32(1);   // liste d'1 compound
    b.tag(1, "Y"); b.u8(sectionY & 0xFF);       // TAG_Byte signe
    b.tag(10, "block_states");
    b.tag(9, "palette"); b.u8(10); b.i32(2);
    b.tag(8, "Name"); b.str("minecraft:air");   b.u8(0);
    b.tag(8, "Name"); b.str("minecraft:stone"); b.u8(0);
    // 2 entrees -> 4 bits mini, 16 valeurs par long, 4096/16 = 256 long.
    b.tag(12, "data"); b.i32(256);
    for (int k = 0; k < 256; ++k) {
        // Chaque long porte 16 indices de 4 bits, tous a 1 (stone), sauf le
        // tout premier indice du premier long, mis a 0 (air) en (0,0,0).
        unsigned long long word = 0;
        for (int s = 0; s < 16; ++s) {
            const unsigned long long val = (k == 0 && s == 0) ? 0ull : 1ull;
            word |= val << (s * 4);
        }
        b.i64(static_cast<long long>(word));
    }
    b.u8(0); // fin block_states
    b.u8(0); // fin du compound de section
    b.u8(0); // fin de la racine
    return b.v;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-region-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp / "region");

    // =====================================================================
    // 1. Nommage des fichiers de region
    // =====================================================================
    {
        auto f = region::parse_region_name("r.0.0.mca");
        CHECK(f.has_value());
        if (f) { CHECK_EQ(f->rx, 0); CHECK_EQ(f->rz, 0); }
        auto g = region::parse_region_name("r.-2.3.mca");
        CHECK(g.has_value());
        if (g) { CHECK_EQ(g->rx, -2); CHECK_EQ(g->rz, 3); }
        CHECK(!region::parse_region_name("r.0.mca").has_value());
        CHECK(!region::parse_region_name("level.dat").has_value());
        CHECK(!region::parse_region_name("r.a.b.mca").has_value());
        CHECK(!region::parse_region_name("r.0.0.mcr").has_value());
    }

    // =====================================================================
    // 2. Region synthetique 1.18+ : en-tete, lecture, blocs, hauteurs
    // =====================================================================
    {
        const fs::path rp = tmp / "region" / "r.0.0.mca";
        CHECK(write_region(rp, 3, 5, make_chunk_modern(0, 3465), 2));

        const auto chunks = region::list_chunks(rp);
        CHECK_EQ(chunks.size(), size_t{1});
        if (chunks.size() == 1) {
            CHECK_EQ(chunks[0].localX, 3);
            CHECK_EQ(chunks[0].localZ, 5);
            CHECK_EQ(chunks[0].cx, 3);
            CHECK_EQ(chunks[0].cz, 5);
            CHECK_EQ(chunks[0].timestamp, 0x2A);
        }

        auto c = region::read_chunk_local(rp, 3, 5);
        CHECK(c.has_value());
        auto absent = region::read_chunk_local(rp, 0, 0);
        CHECK(!absent.has_value());
        CHECK(!region::read_chunk_local(rp, 99, 0).has_value());

        if (c) {
            // (0,0,0) = air, le reste = stone
            CHECK_EQ(region::block_at(*c, 0, 0, 0), std::string("minecraft:air"));
            CHECK_EQ(region::block_at(*c, 1, 0, 0), std::string("minecraft:stone"));
            CHECK_EQ(region::block_at(*c, 15, 15, 15), std::string("minecraft:stone"));
            // hors chunk
            CHECK(region::block_at(*c, 16, 0, 0).empty());
            // section absente a cette altitude
            CHECK(region::block_at(*c, 0, 200, 0).empty());

            const auto hm = region::heightmap(*c);
            CHECK_EQ(hm.size(), size_t{256});
            // colonne (1,0) : stone jusqu'a y=15
            CHECK_EQ(hm[0 * 16 + 1], 15);
            // colonne (0,0) : air en bas mais stone au-dessus -> 15 aussi
            CHECK_EQ(hm[0], 15);

            const auto uniq = region::unique_blocks(*c);
            CHECK_EQ(uniq.size(), size_t{2});
        }

        // lecture par coordonnees monde
        CHECK(region::read_chunk(tmp, 3, 5).has_value());
        CHECK(!region::read_chunk(tmp, 400, 400).has_value());

        // listing des regions
        const auto regs = region::list_regions(tmp);
        CHECK_EQ(regs.size(), size_t{1});
    }

    // =====================================================================
    // 3. Section a Y negatif (le C# la plaçait a y = 4080)
    // =====================================================================
    {
        const fs::path rp = tmp / "region" / "r.1.0.mca";
        CHECK(write_region(rp, 0, 0, make_chunk_modern(-1, 3465), 2));
        auto c = region::read_chunk_local(rp, 0, 0);
        CHECK(c.has_value());
        if (c) {
            // Y = -1 -> blocs de y = -16 a -1
            CHECK_EQ(region::block_at(*c, 1, -16, 0), std::string("minecraft:stone"));
            CHECK_EQ(region::block_at(*c, 1, -1, 0), std::string("minecraft:stone"));
            // rien a y = 4080, contrairement a la lecture non signee du C#
            CHECK(region::block_at(*c, 1, 4080, 0).empty());
            const auto hm = region::heightmap(*c);
            CHECK_EQ(hm[0 * 16 + 1], -1);
        }
    }

    // =====================================================================
    // 4. Chunk non compresse (scheme 3), legal et ignore par le C#
    // =====================================================================
    {
        const fs::path rp = tmp / "region" / "r.2.0.mca";
        CHECK(write_region(rp, 1, 1, make_chunk_modern(0, 3465), 3));
        auto c = region::read_chunk_local(rp, 1, 1);
        CHECK(c.has_value());
        if (c) CHECK_EQ(region::block_at(*c, 1, 0, 0), std::string("minecraft:stone"));
    }

    // =====================================================================
    // 5. Effacement de chunks (selection de l'editeur de cartes)
    // =====================================================================
    {
        const fs::path rp = tmp / "region" / "r.3.0.mca";
        CHECK(write_region(rp, 4, 4, make_chunk_modern(0, 3465), 2));
        CHECK_EQ(region::list_chunks(rp).size(), size_t{1});
        CHECK_EQ(region::clear_chunks(rp, {{4, 4}}), 1);
        CHECK_EQ(region::list_chunks(rp).size(), size_t{0});
        CHECK(!region::read_chunk_local(rp, 4, 4).has_value());
        // deja efface : rien a faire
        CHECK_EQ(region::clear_chunks(rp, {{4, 4}}), 0);
        // coordonnees hors bornes ignorees
        CHECK_EQ(region::clear_chunks(rp, {{99, 0}}), 0);
    }

    // =====================================================================
    // 6. Fichiers invalides : jamais de crash
    // =====================================================================
    {
        const fs::path bad = tmp / "region" / "r.9.9.mca";
        std::ofstream(bad, std::ios::binary) << "trop court";
        CHECK(region::list_chunks(bad).empty());
        CHECK(!region::read_chunk_local(bad, 0, 0).has_value());
        CHECK_EQ(region::clear_chunks(bad, {{0, 0}}), 0);
        CHECK(region::list_chunks(tmp / "region" / "nexistepas.mca").empty());
        // en-tete annoncant un offset hors fichier
        const fs::path trunc = tmp / "region" / "r.8.8.mca";
        {
            std::vector<std::uint8_t> f(8192, 0);
            f[2] = 200; // offset = secteur 200, bien au-dela du fichier
            f[3] = 1;
            std::ofstream o(trunc, std::ios::binary);
            o.write(reinterpret_cast<const char*>(f.data()), 8192);
        }
        CHECK(region::list_chunks(trunc).empty());
        CHECK(!region::read_chunk_local(trunc, 0, 0).has_value());
    }

    // =====================================================================
    // 6bis. Ecriture de chunks : aller-retour, remplacement, agrandissement
    // =====================================================================
    {
        const fs::path rp = tmp / "region" / "r.5.5.mca";
        std::error_code e3;
        fs::remove(rp, e3);

        // Chunk minimal mais valide, ecrit dans une region inexistante.
        nbt::Compound c;
        c["DataVersion"] = nbt::make_int(3465);
        c["Marqueur"] = nbt::make_string("premier");
        CHECK(region::write_chunk_local(rp, 7, 9, c));
        CHECK(fs::exists(rp, e3));
        CHECK(fs::file_size(rp, e3) >= 8192 + 4096);
        // Aucun fichier temporaire ne doit subsister
        CHECK(!fs::exists(fs::path(rp).concat(".tmp"), e3));

        auto back = region::read_chunk_local(rp, 7, 9);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(nbt::get_string(*back, "Marqueur"), std::string("premier"));
            CHECK_EQ(nbt::get_num(*back, "DataVersion"), 3465LL);
        }
        auto chunks = region::list_chunks(rp);
        CHECK_EQ(chunks.size(), size_t{1});
        if (chunks.size() == 1) {
            CHECK_EQ(chunks[0].localX, 7);
            CHECK_EQ(chunks[0].localZ, 9);
            CHECK(chunks[0].timestamp > 0); // horodatage pose
        }

        // Remplacement par un chunk beaucoup plus gros : il doit etre realloue
        // sans ecraser un voisin.
        nbt::Compound d;
        d["Marqueur"] = nbt::make_string("second");
        d["Charge"] = nbt::make_long_array(std::vector<std::int64_t>(20000, 42));
        CHECK(region::write_chunk_local(rp, 7, 9, d));
        auto b2 = region::read_chunk_local(rp, 7, 9);
        CHECK(b2.has_value());
        if (b2) {
            CHECK_EQ(nbt::get_string(*b2, "Marqueur"), std::string("second"));
            const nbt::Tag* t = nbt::find(*b2, "Charge");
            CHECK(t && t->longs.size() == 20000);
            if (t && !t->longs.empty()) CHECK_EQ(t->longs[19999], 42LL);
        }

        // Un second chunk dans la meme region ne doit pas abimer le premier.
        nbt::Compound e;
        e["Marqueur"] = nbt::make_string("voisin");
        CHECK(region::write_chunk_local(rp, 0, 0, e));
        auto b3 = region::read_chunk_local(rp, 0, 0);
        auto b4 = region::read_chunk_local(rp, 7, 9);
        CHECK(b3.has_value());
        CHECK(b4.has_value());
        if (b3) CHECK_EQ(nbt::get_string(*b3, "Marqueur"), std::string("voisin"));
        if (b4) CHECK_EQ(nbt::get_string(*b4, "Marqueur"), std::string("second"));
        CHECK_EQ(region::list_chunks(rp).size(), size_t{2});

        // Coordonnees hors bornes refusees
        CHECK(!region::write_chunk_local(rp, 32, 0, c));
        CHECK(!region::write_chunk_local(rp, -1, 0, c));

        // Ecriture par coordonnees monde, region creee au besoin
        CHECK(region::write_chunk(tmp, -33, 5, c));
        auto b5 = region::read_chunk(tmp, -33, 5);
        CHECK(b5.has_value());
        if (b5) CHECK_EQ(nbt::get_string(*b5, "Marqueur"), std::string("premier"));
    }

    // =====================================================================
    // 6ter. Modification de blocs : palette reconstruite, aller-retour disque
    // =====================================================================
    {
        const fs::path rp = tmp / "region" / "r.6.6.mca";
        std::error_code e4;
        fs::remove(rp, e4);
        CHECK(write_region(rp, 2, 3, make_chunk_modern(0, 3465), 2));
        auto c = region::read_chunk_local(rp, 2, 3);
        CHECK(c.has_value());
        if (c) {
            // Depart : (0,0,0) air, le reste stone.
            CHECK_EQ(region::block_at(*c, 0, 0, 0), std::string("minecraft:air"));
            std::vector<region::BlockEdit> edits = {
                {0, 0, 0, "minecraft:gold_block"},   // nouveau nom -> palette
                {5, 3, 7, "minecraft:water"},
                {1, 0, 0, "minecraft:air"},          // nom deja en palette
                {99, 0, 0, "minecraft:stone"},       // hors chunk -> ignore
                {0, 500, 0, "minecraft:stone"},      // section absente -> ignore
            };
            const auto r = region::set_blocks(*c, edits);
            CHECK_EQ(r.applied, 3);
            CHECK_EQ(r.skipped, 2);
            CHECK_EQ(r.unsupported, 0);

            // Relecture en memoire
            CHECK_EQ(region::block_at(*c, 0, 0, 0),
                     std::string("minecraft:gold_block"));
            CHECK_EQ(region::block_at(*c, 5, 3, 7), std::string("minecraft:water"));
            CHECK_EQ(region::block_at(*c, 1, 0, 0), std::string("minecraft:air"));
            // Un bloc non touche garde sa valeur
            CHECK_EQ(region::block_at(*c, 15, 15, 15), std::string("minecraft:stone"));

            // Aller-retour disque : la section re-encodee doit se relire
            CHECK(region::write_chunk_local(rp, 2, 3, *c));
            auto d = region::read_chunk_local(rp, 2, 3);
            CHECK(d.has_value());
            if (d) {
                CHECK_EQ(region::block_at(*d, 0, 0, 0),
                         std::string("minecraft:gold_block"));
                CHECK_EQ(region::block_at(*d, 5, 3, 7),
                         std::string("minecraft:water"));
                CHECK_EQ(region::block_at(*d, 15, 15, 15),
                         std::string("minecraft:stone"));
                const auto uniq = region::unique_blocks(*d);
                CHECK_EQ(uniq.size(), size_t{4}); // air, stone, gold, water
            }
        }

        // Table 1.12
        CHECK_EQ(region::legacy_id_for("minecraft:stone"), 1);
        CHECK_EQ(region::legacy_id_for("minecraft:water"), 9);
        CHECK_EQ(region::legacy_id_for("minecraft:air"), 0);
        CHECK_EQ(region::legacy_id_for("minecraft:id_57"), 57);
        CHECK_EQ(region::legacy_id_for("minecraft:inconnu"), -1);

        // Chunk sans sections : tout est ignore, rien ne plante
        nbt::Compound vide;
        const auto rv = region::set_blocks(vide, {{0, 0, 0, "minecraft:stone"}});
        CHECK_EQ(rv.applied, 0);
        CHECK_EQ(rv.skipped, 1);
    }

    // =====================================================================
    // 7. Monde reel de la machine, s'il y en a un
    // =====================================================================
    {
        const char* home = std::getenv("LOCALAPPDATA");
        bool tested = false;
        if (home) {
            const fs::path root = fs::path(home) / "TeamLauncher" / "instances";
            std::error_code e2;
            if (fs::is_directory(root, e2)) {
                for (fs::recursive_directory_iterator it(root, e2), end;
                     it != end && !tested; it.increment(e2)) {
                    if (e2) break;
                    if (!it->is_directory(e2)) continue;
                    if (it->path().filename() != "region") continue;
                    const auto regs = region::list_regions(it->path().parent_path());
                    if (regs.empty()) continue;
                    const auto chunks = region::list_chunks(regs[0].path);
                    std::printf("INFO monde reel : %zu region(s), %zu chunk(s) "
                                "dans %s\n",
                                regs.size(), chunks.size(),
                                regs[0].path.filename().string().c_str());
                    CHECK(!chunks.empty());
                    if (!chunks.empty()) {
                        auto c = region::read_chunk_local(regs[0].path,
                                                          chunks[0].localX,
                                                          chunks[0].localZ);
                        CHECK(c.has_value());
                        if (c) {
                            const auto uniq = region::unique_blocks(*c);
                            const auto hm = region::heightmap(*c);
                            int filled = 0;
                            for (auto h : hm)
                                if (h != INT32_MIN) ++filled;
                            std::printf("INFO chunk (%d,%d) : %zu bloc(s) "
                                        "distinct(s), %d/256 colonnes\n",
                                        chunks[0].cx, chunks[0].cz, uniq.size(),
                                        filled);
                            CHECK(!uniq.empty());
                            CHECK(filled > 0);
                        }
                    }
                    tested = true;
                }
            }
        }
        if (!tested) std::printf("INFO aucun monde reel trouve (test saute)\n");
    }

    fs::remove_all(tmp, ec);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

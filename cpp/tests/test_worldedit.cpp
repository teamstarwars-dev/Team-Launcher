// Tests des operations WorldEdit : bornes de selection, //set, //replace,
// //copy, //paste, //undo, //redo, plafond de volume, chunk absent.
//
// Tout se passe sur un monde synthetique ecrit dans le dossier temporaire,
// via les fonctions deja validees de nbt/region.

#include "worldedit.hpp"

#include "nbt.hpp"
#include "region.hpp"

#include <cstdio>
#include <cmath> // std::floor (MSVC l'obtient par transitivité, pas GCC)
#include <filesystem>
#include <string>

using namespace tl::worldedit;
namespace fs = std::filesystem;
namespace nbt = tl::nbt;
namespace region = tl::region;

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

// Chunk 1.18+ d'une seule section (y 0-15), entierement en pierre.
static void write_chunk(const fs::path& world, int cx, int cz) {
    nbt::Compound sec;
    sec["Y"] = nbt::make_byte(0);
    nbt::Compound bs;
    nbt::List palette;
    for (const char* n : {"minecraft:air", "minecraft:stone"}) {
        nbt::Compound e;
        e["Name"] = nbt::make_string(n);
        palette.push_back(nbt::make_compound(std::move(e)));
    }
    bs["palette"] = nbt::make_list(nbt::Type::Compound, std::move(palette));
    std::vector<std::int64_t> data(
        256, static_cast<std::int64_t>(0x1111111111111111ull));
    bs["data"] = nbt::make_long_array(std::move(data));
    sec["block_states"] = nbt::make_compound(std::move(bs));

    nbt::List sections;
    sections.push_back(nbt::make_compound(std::move(sec)));
    nbt::Compound chunk;
    chunk["DataVersion"] = nbt::make_int(3465);
    chunk["sections"] = nbt::make_list(nbt::Type::Compound, std::move(sections));

    const int rx = static_cast<int>(std::floor(cx / 32.0));
    const int rz = static_cast<int>(std::floor(cz / 32.0));
    const fs::path rf = world / "region" /
                        ("r." + std::to_string(rx) + "." + std::to_string(rz) +
                         ".mca");
    if (!region::write_chunk_local(rf, cx - rx * 32, cz - rz * 32, chunk)) {
        std::printf("FAIL: ecriture du chunk de test (%d,%d)\n", cx, cz);
        ++g_failures;
    }
}

static std::string at(const fs::path& world, int x, int y, int z) {
    const int cx = static_cast<int>(std::floor(x / 16.0));
    const int cz = static_cast<int>(std::floor(z / 16.0));
    auto c = region::read_chunk(world, cx, cz);
    if (!c) return "<absent>";
    return region::block_at(*c, ((x % 16) + 16) % 16, y, ((z % 16) + 16) % 16);
}

int main() {
    // =====================================================================
    // 1. Bornes
    // =====================================================================
    {
        const auto b = bounds_of({10, 5, -3}, {2, 9, 4});
        CHECK_EQ(b.x1, 2);
        CHECK_EQ(b.x2, 10);
        CHECK_EQ(b.y1, 5);
        CHECK_EQ(b.y2, 9);
        CHECK_EQ(b.z1, -3);
        CHECK_EQ(b.z2, 4);
        CHECK_EQ(b.volume(), std::int64_t{9} * 5 * 8);
        // Un point isole a un volume de 1 (bornes inclusives)
        CHECK_EQ(bounds_of({0, 0, 0}, {0, 0, 0}).volume(), std::int64_t{1});
    }

    const fs::path world = fs::temp_directory_path() / "tl-worldedit-test";
    std::error_code ec;
    fs::remove_all(world, ec);
    fs::create_directories(world / "region");

    // =====================================================================
    // 2. Refus : monde absent, volume excessif, arguments vides
    // =====================================================================
    {
        History h;
        const auto noWorld = set_region(fs::temp_directory_path() / "tl-we-absent",
                                        bounds_of({0, 0, 0}, {1, 1, 1}),
                                        "minecraft:stone", &h);
        CHECK(!noWorld.error.empty());

        // Un cube de 300 de cote depasse le plafond de 8 millions.
        const auto huge = set_region(world, bounds_of({0, 0, 0}, {299, 299, 299}),
                                     "minecraft:stone", &h);
        CHECK(!huge.error.empty());
        CHECK_EQ(huge.changed, 0);

        CHECK(!set_region(world, bounds_of({0, 0, 0}, {1, 1, 1}), "", &h)
                   .error.empty());
        CHECK(!replace_region(world, bounds_of({0, 0, 0}, {1, 1, 1}),
                              "minecraft:stone", "", &h)
                   .error.empty());
        CHECK(!h.can_undo()); // aucun refus n'a empile d'annulation
    }

    // =====================================================================
    // 3. //set sur deux chunks, avec historique
    // =====================================================================
    write_chunk(world, 0, 0);
    write_chunk(world, 1, 0);
    History hist;
    {
        // Cuboide a cheval sur les chunks 0 et 1 en X.
        const auto b = bounds_of({14, 2, 3}, {17, 4, 5});
        CHECK_EQ(b.volume(), std::int64_t{4} * 3 * 3);
        const auto r = set_region(world, b, "minecraft:gold_block", &hist);
        CHECK(r.error.empty());
        CHECK_EQ(r.changed, 36);
        CHECK_EQ(r.chunks, 2);
        CHECK_EQ(r.unsupported, 0);

        CHECK_EQ(at(world, 14, 2, 3), std::string("minecraft:gold_block"));
        CHECK_EQ(at(world, 17, 4, 5), std::string("minecraft:gold_block"));
        CHECK_EQ(at(world, 13, 2, 3), std::string("minecraft:stone")); // hors
        CHECK_EQ(at(world, 18, 2, 3), std::string("minecraft:stone"));
        CHECK(hist.can_undo());
        CHECK_EQ(hist.undo_count(), std::size_t{1});
        CHECK(!hist.can_redo());
    }

    // =====================================================================
    // 4. //undo puis //redo
    // =====================================================================
    {
        const auto u = hist.undo(world);
        CHECK(u.error.empty());
        CHECK_EQ(u.chunks, 2);
        CHECK_EQ(at(world, 14, 2, 3), std::string("minecraft:stone"));
        CHECK_EQ(at(world, 17, 4, 5), std::string("minecraft:stone"));
        CHECK(!hist.can_undo());
        CHECK(hist.can_redo());

        const auto rd = hist.redo(world);
        CHECK(rd.error.empty());
        CHECK_EQ(at(world, 14, 2, 3), std::string("minecraft:gold_block"));
        CHECK(hist.can_undo());
        CHECK(!hist.can_redo());

        // Pile vide : message, pas de plantage
        History empty;
        CHECK(!empty.undo(world).error.empty());
        CHECK(!empty.redo(world).error.empty());
    }

    // =====================================================================
    // 5. //replace, y compris le joker "*"
    // =====================================================================
    {
        const auto b = bounds_of({14, 2, 3}, {17, 4, 5});
        // L'or repasse en pierre : seuls les blocs d'or comptent.
        const auto r = replace_region(world, b, "minecraft:gold_block",
                                      "minecraft:stone", &hist);
        CHECK(r.error.empty());
        CHECK_EQ(r.changed, 36);
        CHECK_EQ(at(world, 14, 2, 3), std::string("minecraft:stone"));

        // Aucun bloc ne correspond : rien n'est ecrit
        const auto none = replace_region(world, b, "minecraft:diamond_block",
                                         "minecraft:stone", &hist);
        CHECK(none.error.empty());
        CHECK_EQ(none.changed, 0);
        CHECK_EQ(none.chunks, 0);

        // Joker : tout ce qui n'est pas deja de la brique
        const auto all =
            replace_region(world, bounds_of({0, 0, 0}, {3, 1, 3}), "*",
                           "minecraft:bricks", &hist);
        CHECK(all.error.empty());
        // 4x2x4 = 32 cases, dont (0,0,0) qui etait de l'air : tout change.
        CHECK_EQ(all.changed, 32);
        CHECK_EQ(at(world, 0, 0, 0), std::string("minecraft:bricks"));
        CHECK_EQ(at(world, 3, 1, 3), std::string("minecraft:bricks"));

        // Rejouer le joker sur la meme zone ne change plus rien (`to` exclu).
        const auto again =
            replace_region(world, bounds_of({0, 0, 0}, {3, 1, 3}), "*",
                           "minecraft:bricks", &hist);
        CHECK_EQ(again.changed, 0);
    }

    // =====================================================================
    // 6. //copy et //paste : le coin minimal fait l'origine
    // =====================================================================
    {
        // Motif reconnaissable dans le chunk 0.
        set_region(world, bounds_of({1, 3, 1}, {2, 3, 2}), "minecraft:diamond_block",
                   &hist);

        Clipboard clip;
        // La selection commence en (0,3,0), qui est de la PIERRE a ce stade —
        // mais on englobe aussi de l'air plus haut pour verifier que le coin
        // minimal reste l'origine meme s'il n'est pas copie.
        const auto cr = copy_region(world, bounds_of({0, 3, 0}, {3, 3, 3}), clip);
        CHECK(cr.error.empty());
        CHECK_EQ(clip.sizeX, 4);
        CHECK_EQ(clip.sizeY, 1);
        CHECK_EQ(clip.sizeZ, 4);
        CHECK_EQ(clip.blocks.size(), std::size_t{16}); // aucun air a y=3
        // Les coordonnees sont RELATIVES : aucune ne doit sortir de la boite.
        for (const auto& [p, n] : clip.blocks) {
            CHECK(p.x >= 0 && p.x < clip.sizeX);
            CHECK(p.y >= 0 && p.y < clip.sizeY);
            CHECK(p.z >= 0 && p.z < clip.sizeZ);
            CHECK(n != "minecraft:air");
        }

        // Collage decale de 8 en X : le diamant doit atterrir en (9..10, 7, 1..2).
        const auto pr = paste(world, clip, Pos{8, 7, 0}, &hist);
        CHECK(pr.error.empty());
        CHECK_EQ(pr.changed, 16);
        CHECK_EQ(at(world, 9, 7, 1), std::string("minecraft:diamond_block"));
        CHECK_EQ(at(world, 10, 7, 2), std::string("minecraft:diamond_block"));
        CHECK_EQ(at(world, 8, 7, 0), std::string("minecraft:stone"));
        // Hors du motif colle : inchange
        CHECK_EQ(at(world, 12, 7, 0), std::string("minecraft:stone"));

        // Annuler le collage remet la pierre.
        CHECK(hist.undo(world).error.empty());
        CHECK_EQ(at(world, 9, 7, 1), std::string("minecraft:stone"));

        // Presse-papier vide
        CHECK(!paste(world, Clipboard{}, Pos{0, 0, 0}, &hist).error.empty());
    }

    // =====================================================================
    // 7. Chunk jamais genere : ignore, jamais fabrique
    // =====================================================================
    {
        const auto r = set_region(world, bounds_of({1000, 2, 1000}, {1001, 2, 1001}),
                                  "minecraft:stone", nullptr);
        CHECK(r.error.empty());
        CHECK_EQ(r.changed, 0);
        CHECK_EQ(r.chunks, 0);
        CHECK_EQ(r.skipped, 4);
        CHECK(!fs::exists(world / "region" / "r.1.1.mca"));
    }

    // =====================================================================
    // 8. Hors des sections : compte en ignore, rien n'est cree
    // =====================================================================
    {
        const auto r = set_region(world, bounds_of({0, 200, 0}, {1, 200, 1}),
                                  "minecraft:stone", nullptr);
        CHECK(r.error.empty());
        CHECK_EQ(r.changed, 0);
        CHECK_EQ(r.skipped, 4);
    }

    // =====================================================================
    // 9. Annulation cooperative
    // =====================================================================
    {
        std::atomic<bool> cancel{true};
        const auto r = set_region(world, bounds_of({0, 0, 0}, {1, 1, 1}),
                                  "minecraft:stone", nullptr, {}, &cancel);
        CHECK(!r.error.empty());
        CHECK_EQ(r.changed, 0);
    }

    // =====================================================================
    // 10. Historique borne : les etapes les plus anciennes partent
    // =====================================================================
    {
        History h;
        for (std::size_t i = 0; i < History::kMaxSteps + 5; ++i) {
            const int y = 1 + static_cast<int>(i % 3);
            const auto r = set_region(
                world, bounds_of({0, y, 0}, {0, y, 0}),
                (i % 2) ? "minecraft:stone" : "minecraft:cobblestone", &h);
            CHECK(r.error.empty());
        }
        CHECK_EQ(h.undo_count(), History::kMaxSteps);
        CHECK(h.bytes() <= History::kMaxBytes);
    }

    // =====================================================================
    // 11. ChunkView : meme reponse que block_at, sur toutes les positions
    // =====================================================================
    {
        auto c = region::read_chunk(world, 0, 0);
        CHECK(c.has_value());
        if (c) {
            const region::ChunkView v(*c);
            int mismatch = 0;
            for (int x = 0; x < 16; x += 3)
                for (int z = 0; z < 16; z += 3)
                    for (int y = -2; y < 20; ++y)
                        if (v.at(x, y, z) != region::block_at(*c, x, y, z))
                            ++mismatch;
            CHECK_EQ(mismatch, 0);
            // Hors du chunk
            CHECK_EQ(v.at(-1, 0, 0), std::string(""));
            CHECK_EQ(v.at(16, 0, 0), std::string(""));
            // Section absente
            CHECK_EQ(v.at(0, 500, 0), std::string(""));
        }
    }

    fs::remove_all(world, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

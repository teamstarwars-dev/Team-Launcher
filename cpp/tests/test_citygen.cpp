// Tests du generateur de ville : emprise, analyse d'une reponse Overpass
// (fabriquee ici, donc aucun appel reseau), classement des entites,
// projection, et rasterisation.

#include "citygen.hpp"

#include "region.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

using namespace tl::citygen;
using nlohmann::json;
namespace fs = std::filesystem;
namespace nbt = tl::nbt;

static int g_failures = 0;

// Chunk 1.18+ d'une seule section (y 0-15), entierement en pierre. Ecrit par
// les fonctions deja validees de region/nbt : le test porte sur le collage,
// pas sur la fabrication du fichier.
static void write_modern_chunk(const fs::path& regionFile, int lx, int lz) {
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
    // 2 entrees -> 4 bits, 16 indices par long, tous a 1 (pierre).
    std::vector<std::int64_t> data(256,
                                   static_cast<std::int64_t>(0x1111111111111111ull));
    bs["data"] = nbt::make_long_array(std::move(data));
    sec["block_states"] = nbt::make_compound(std::move(bs));

    nbt::List sections;
    sections.push_back(nbt::make_compound(std::move(sec)));

    nbt::Compound chunk;
    chunk["DataVersion"] = nbt::make_int(3465);
    chunk["sections"] = nbt::make_list(nbt::Type::Compound, std::move(sections));
    if (!tl::region::write_chunk_local(regionFile, lx, lz, chunk)) {
        std::printf("FAIL: impossible d'ecrire le chunk de test\n");
        ++g_failures;
    }
}

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

// Construit une reponse Overpass : noeuds puis chemins.
static std::string overpass(const json& elements) {
    return json{{"elements", elements}}.dump();
}

static json node(long long id, double lon, double lat) {
    return json{{"type", "node"}, {"id", id}, {"lon", lon}, {"lat", lat}};
}

static json way(long long id, std::vector<long long> nodeIds, json tags) {
    return json{{"type", "way"}, {"id", id}, {"nodes", nodeIds}, {"tags", tags}};
}

int main() {
    // =====================================================================
    // 1. Emprise
    // =====================================================================
    {
        auto b = parse_bbox("2.29,48.85,2.30,48.86");
        CHECK(b.valid);
        if (b.valid) {
            CHECK(std::fabs(b.minLon - 2.29) < 1e-9);
            CHECK(std::fabs(b.maxLat - 48.86) < 1e-9);
        }
        CHECK(parse_bbox(" 2.29 , 48.85 , 2.30 , 48.86 ").valid); // espaces
        CHECK(!parse_bbox("2.29,48.85,2.30").valid);        // trois champs
        CHECK(!parse_bbox("").valid);
        CHECK(!parse_bbox("a,b,c,d").valid);                 // non numerique
        CHECK(!parse_bbox("2.30,48.85,2.29,48.86").valid);   // min >= max
        CHECK(!parse_bbox("2.29,48.86,2.30,48.85").valid);
        CHECK(!parse_bbox("-200,48.85,2.30,48.86").valid);   // hors bornes
        CHECK(!parse_bbox("2.29,-91,2.30,48.86").valid);
        CHECK(!parse_bbox("2.29,48.85,2.30,").valid);        // champ vide
    }

    const BBox box = parse_bbox("2.00,48.00,2.02,48.02");
    CHECK(box.valid);

    // =====================================================================
    // 2. Analyse et classement
    // =====================================================================
    {
        json els = json::array();
        // Carre de 4 noeuds autour du centre
        els.push_back(node(1, 2.005, 48.005));
        els.push_back(node(2, 2.015, 48.005));
        els.push_back(node(3, 2.015, 48.015));
        els.push_back(node(4, 2.005, 48.015));
        els.push_back(way(100, {1, 2, 3, 4, 1},
                          json{{"building", "yes"}, {"building:levels", "3"}}));
        els.push_back(way(101, {1, 2}, json{{"highway", "primary"}}));
        els.push_back(way(102, {2, 3}, json{{"highway", "residential"}}));
        els.push_back(way(103, {3, 4}, json{{"natural", "water"}}));
        els.push_back(way(104, {1, 3}, json{{"railway", "rail"}}));
        els.push_back(way(105, {1, 2, 3}, json{{"leisure", "park"}}));
        // CORRECTIF : natural=wood ne doit PAS devenir de l'eau (le C#
        // testait la seule presence du tag « natural »).
        els.push_back(way(106, {2, 4}, json{{"natural", "wood"}}));
        // Chemin sans tag exploitable : ignore
        els.push_back(way(107, {1, 2}, json{{"foo", "bar"}}));
        // Chemin avec un seul point resolvable : ignore
        els.push_back(way(108, {1, 999}, json{{"building", "yes"}}));

        const auto d = parse_overpass(overpass(els), box);
        CHECK_EQ(d.nodes, 4);
        CHECK(std::fabs(d.centerLon - 2.01) < 1e-9);
        CHECK(std::fabs(d.centerLat - 48.01) < 1e-9);

        auto by_id = [&](long long id) -> const Entity* {
            for (const auto& e : d.entities)
                if (e.id == id) return &e;
            return nullptr;
        };
        CHECK(by_id(100) && by_id(100)->kind == Kind::Building);
        if (by_id(100)) CHECK_EQ(by_id(100)->height, 12); // 3 niveaux x 4
        CHECK(by_id(101) && by_id(101)->kind == Kind::Highway);
        if (by_id(101)) CHECK_EQ(by_id(101)->width, 6);   // voie primaire
        CHECK(by_id(102) && by_id(102)->width == 3);      // voie residentielle
        CHECK(by_id(103) && by_id(103)->kind == Kind::Water);
        CHECK(by_id(104) && by_id(104)->kind == Kind::Railway);
        CHECK(by_id(105) && by_id(105)->kind == Kind::Park);
        CHECK(by_id(106) == nullptr); // natural=wood : pas de l'eau, pas retenu
        CHECK(by_id(107) == nullptr);
        CHECK(by_id(108) == nullptr);

        // Projection : le centre tombe a l'origine, et l'est est en x positif.
        const Entity* b100 = by_id(100);
        CHECK(b100 && b100->points.size() == 5);
        if (b100 && b100->points.size() == 5) {
            CHECK(b100->points[0].x < 0); // lon 2.005 < centre 2.01
            CHECK(b100->points[1].x > 0); // lon 2.015 > centre
            CHECK(b100->points[0].z < 0); // lat 48.005 < centre 48.01
            CHECK(b100->points[2].z > 0);
            // ~0.005 degre de latitude ~ 556 m
            CHECK(std::abs(b100->points[0].z) > 400);
            CHECK(std::abs(b100->points[0].z) < 700);
            // La longitude est resserree par cos(48 degres) ~ 0,669
            CHECK(std::abs(b100->points[0].x) < std::abs(b100->points[0].z));
        }
    }

    // =====================================================================
    // 3. Entrees degradees : jamais de plantage
    // =====================================================================
    {
        CHECK(parse_overpass("pas du json", box).entities.empty());
        CHECK(parse_overpass("{}", box).entities.empty());
        CHECK(parse_overpass("{\"elements\":\"pas un tableau\"}", box)
                  .entities.empty());
        CHECK(parse_overpass(overpass(json::array()), box).entities.empty());
        // Emprise invalide : rien
        CHECK(parse_overpass(overpass(json::array()), BBox{}).entities.empty());
    }

    // =====================================================================
    // 4. Geometrie
    // =====================================================================
    {
        // Carre de 0,0 a 10,10
        const std::vector<Point> sq = {{0, 0}, {10, 0}, {10, 10}, {0, 10}};
        CHECK(point_in_polygon(5, 5, sq));
        CHECK(!point_in_polygon(15, 5, sq));
        CHECK(!point_in_polygon(-1, 5, sq));
        CHECK(!point_in_polygon(5, 20, sq));

        std::vector<Block> out;
        fill_polygon(out, sq, 64, 3, "minecraft:stone");
        CHECK(!out.empty());
        // Hauteur : 3 couches
        int atY[3] = {0, 0, 0};
        for (const auto& b : out) {
            CHECK(b.y >= 64);
            CHECK(b.y <= 66);
            if (b.y >= 64 && b.y <= 66) ++atY[b.y - 64];
        }
        CHECK(atY[0] > 0 && atY[0] == atY[1] && atY[1] == atY[2]);

        // Polygone degenere
        out.clear();
        fill_polygon(out, {{0, 0}, {1, 1}}, 64, 1, "x");
        CHECK(out.empty());

        // Emprise aberrante : abandonnee, pas de boucle infinie
        out.clear();
        fill_polygon(out, {{0, 0}, {5000000, 0}, {5000000, 5000000}}, 64, 1, "x");
        CHECK(out.empty());

        // Ligne
        out.clear();
        fill_line(out, {{0, 0}, {10, 0}}, 3, 64, "minecraft:gravel");
        CHECK(!out.empty());
        for (const auto& b : out) {
            CHECK_EQ(b.y, 64);
            CHECK(b.z >= -1 && b.z <= 1); // largeur 3 -> demi-largeur 1
        }
        // Un seul point : rien
        out.clear();
        fill_line(out, {{0, 0}}, 3, 64, "x");
        CHECK(out.empty());
    }

    // =====================================================================
    // 5. Rasterisation complete et plafond
    // =====================================================================
    {
        json els = json::array();
        els.push_back(node(1, 2.0090, 48.0090));
        els.push_back(node(2, 2.0110, 48.0090));
        els.push_back(node(3, 2.0110, 48.0110));
        els.push_back(node(4, 2.0090, 48.0110));
        els.push_back(way(200, {1, 2, 3, 4, 1}, json{{"building", "yes"}}));
        const auto d = parse_overpass(overpass(els), box);
        CHECK_EQ(d.entities.size(), size_t{1});

        const auto blocks = rasterize(d, 64);
        CHECK(!blocks.empty());
        for (const auto& b : blocks) {
            CHECK(b.name != nullptr);
            if (b.name) CHECK_EQ(std::string(b.name), std::string("minecraft:stone"));
        }
        std::printf("INFO rasterisation : %zu blocs pour 1 batiment\n",
                    blocks.size());

        // Plafond respecte
        const auto capped = rasterize(d, 64, 100);
        CHECK(capped.size() <= 100);

        // Jeu vide
        CHECK(rasterize(OsmData{}, 64).empty());
    }

    // =====================================================================
    // 6. Ecriture dans un monde : aller-retour disque complet
    // =====================================================================
    {
        const fs::path world = fs::temp_directory_path() / "tl-citygen-test";
        std::error_code ec;
        fs::remove_all(world, ec);
        fs::create_directories(world / "region");

        // Monde absent : refus propre, pas de plantage
        {
            const auto miss = paste_into_world(
                fs::temp_directory_path() / "tl-citygen-absent",
                {{0, 64, 0, "minecraft:stone"}}, 0, 0, nullptr);
            CHECK(!miss.error.empty());
            CHECK_EQ(miss.placed, 0);
        }
        // Rien a poser : succes immediat
        CHECK(paste_into_world(world, {}, 0, 0, nullptr).error.empty());

        // Un seul chunk genere, en (0,0) ; la ville est posee dessus.
        write_modern_chunk(world / "region" / "r.0.0.mca", 0, 0);

        std::vector<Block> blocks;
        for (int x = 0; x < 4; ++x)
            for (int z = 0; z < 4; ++z)
                blocks.push_back({x, 5, z, "minecraft:bricks"});
        // Hors du chunk genere : compte en absent, aucun chunk cree
        blocks.push_back({100, 5, 100, "minecraft:bricks"});
        // Hors des sections existantes (section 0 seule) : ignore
        blocks.push_back({1, 200, 1, "minecraft:bricks"});

        std::string last;
        const auto pr = paste_into_world(
            world, blocks, 0, 0, [&](const std::string& m) { last = m; });
        CHECK(pr.error.empty());
        CHECK_EQ(pr.placed, 16);
        CHECK_EQ(pr.skipped, 2);      // 1 chunk absent + 1 section absente
        CHECK_EQ(pr.chunksWritten, 1);
        CHECK_EQ(pr.chunksMissing, 1);
        CHECK(!last.empty());
        // Aucun fichier de region n'a ete cree pour le chunk manquant
        CHECK(!fs::exists(world / "region" / "r.6.6.mca"));

        // Relecture depuis le disque
        auto back = tl::region::read_chunk(world, 0, 0);
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(tl::region::block_at(*back, 0, 5, 0),
                     std::string("minecraft:bricks"));
            CHECK_EQ(tl::region::block_at(*back, 3, 5, 3),
                     std::string("minecraft:bricks"));
            CHECK_EQ(tl::region::block_at(*back, 4, 5, 4),
                     std::string("minecraft:stone")); // hors emprise
        }

        // Coordonnees negatives : le modulo doit rester dans 0-15.
        write_modern_chunk(world / "region" / "r.-1.-1.mca", 31, 31);
        const auto neg =
            paste_into_world(world, {{-1, 5, -1, "minecraft:bricks"}}, 0, 0,
                             nullptr);
        CHECK(neg.error.empty());
        CHECK_EQ(neg.placed, 1);
        auto bn = tl::region::read_chunk(world, -1, -1);
        CHECK(bn.has_value());
        if (bn)
            CHECK_EQ(tl::region::block_at(*bn, 15, 5, 15),
                     std::string("minecraft:bricks"));

        // Annulation
        std::atomic<bool> cancel{true};
        const auto cr = paste_into_world(
            world, {{0, 5, 0, "minecraft:stone"}}, 0, 0, nullptr, &cancel);
        CHECK(!cr.error.empty());
        CHECK_EQ(cr.placed, 0);

        fs::remove_all(world, ec);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

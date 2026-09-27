// Tests de lecture des modeles 3D : les trois formats a base de boites,
// entrees degradees, bornes, et le mannequin de demonstration.

#include "model3d.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace tl::model3d;
namespace fs = std::filesystem;
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

int main() {
    // =====================================================================
    // 1. Modele Java : elements avec from/to
    // =====================================================================
    {
        const json j = {
            {"elements",
             json::array(
                 {json{{"from", json::array({0, 0, 0})},
                       {"to", json::array({16, 16, 16})}},
                  // Ordre inverse : le port normalise, le C# obtenait des
                  // dimensions negatives et jetait la boite.
                  json{{"from", json::array({8, 8, 8})},
                       {"to", json::array({2, 4, 6})}},
                  // Boite plate : ecartee (regle du C# conservee)
                  json{{"from", json::array({0, 0, 0})},
                       {"to", json::array({4, 0, 4})}},
                  // Sans from/to ni origin/size : ignoree
                  json{{"name", "vide"}}})}};
        const auto m = parse(j.dump(), ".json");
        CHECK_EQ(m.boxes.size(), std::size_t{2});
        CHECK_EQ(m.format, std::string("java"));
        if (m.boxes.size() == 2) {
            CHECK(std::fabs(m.boxes[0].w - 16.0f) < 1e-5f);
            // from/to inverses -> coin minimal (2,4,6), taille (6,4,2)
            CHECK(std::fabs(m.boxes[1].x - 2.0f) < 1e-5f);
            CHECK(std::fabs(m.boxes[1].y - 4.0f) < 1e-5f);
            CHECK(std::fabs(m.boxes[1].z - 6.0f) < 1e-5f);
            CHECK(std::fabs(m.boxes[1].w - 6.0f) < 1e-5f);
            CHECK(std::fabs(m.boxes[1].h - 4.0f) < 1e-5f);
            CHECK(std::fabs(m.boxes[1].d - 2.0f) < 1e-5f);
        }
    }

    // =====================================================================
    // 2. Couleurs : explicite, teinte de feuillage, repli
    // =====================================================================
    {
        const json j = {
            {"elements",
             json::array(
                 {json{{"from", json::array({0, 0, 0})},
                       {"to", json::array({1, 1, 1})},
                       {"color", 0xFF8000}},
                  json{{"from", json::array({0, 0, 0})},
                       {"to", json::array({1, 1, 1})},
                       {"faces", {{"up", {{"tintindex", 0}}}}}},
                  json{{"from", json::array({0, 0, 0})},
                       {"to", json::array({1, 1, 1})}}})}};
        const auto m = parse(j.dump(), ".json");
        CHECK_EQ(m.boxes.size(), std::size_t{3});
        if (m.boxes.size() == 3) {
            CHECK_EQ(m.boxes[0].color & 0x00FFFFFFu, 0xFF8000u);
            CHECK_EQ(m.boxes[1].color & 0x00FFFFFFu, 0x8CB464u); // 140,180,100
            CHECK(m.boxes[2].color != m.boxes[1].color);
        }
    }

    // =====================================================================
    // 3. Blockbench ancien : cubes avec origin + size
    // =====================================================================
    {
        const json j = {
            {"meta", {{"format_version", "4.5"}}},
            {"cubes", json::array({json{{"origin", json::array({1, 2, 3})},
                                        {"size", json::array({4, 5, 6})}}})}};
        const auto m = parse(j.dump(), ".bbmodel");
        CHECK_EQ(m.boxes.size(), std::size_t{1});
        CHECK_EQ(m.format, std::string("bbmodel"));
        if (!m.boxes.empty()) {
            CHECK(std::fabs(m.boxes[0].x - 1.0f) < 1e-5f);
            CHECK(std::fabs(m.boxes[0].d - 6.0f) < 1e-5f);
        }
        // La cle « cubes » avec espace en tete du C# n'est PLUS acceptee.
        const json weird = {
            {" cubes", json::array({json{{"origin", json::array({0, 0, 0})},
                                         {"size", json::array({1, 1, 1})}}})}};
        CHECK(parse(weird.dump(), ".bbmodel").empty());
    }

    // =====================================================================
    // 4. Geometrie Bedrock
    // =====================================================================
    {
        const json j = {
            {"format_version", "1.12.0"},
            {"minecraft:geometry",
             json::array({json{
                 {"description", {{"identifier", "geometry.test"}}},
                 {"bones",
                  json::array(
                      {json{{"name", "tete"},
                            {"cubes", json::array({json{
                                          {"origin", json::array({-4, 24, -4})},
                                          {"size", json::array({8, 8, 8})}}})}},
                       json{{"name", "os_sans_cube"}}})}}})}};
        auto m = parse(j.dump(), "model.geo.json");
        CHECK_EQ(m.boxes.size(), std::size_t{1});
        CHECK_EQ(m.format, std::string("bedrock"));
        CHECK_EQ(m.name, std::string("geometry.test"));
        // Meme sans l'extension, la cle doit suffire.
        m = parse(j.dump(), ".json");
        CHECK_EQ(m.format, std::string("bedrock"));
    }

    // =====================================================================
    // 5. Rotation : conservee comme boite, mais signalee
    // =====================================================================
    {
        const json j = {
            {"elements",
             json::array({json{{"from", json::array({0, 0, 0})},
                               {"to", json::array({2, 2, 2})},
                               {"rotation",
                                {{"angle", 22.5}, {"axis", "y"}}}}})}};
        const auto m = parse(j.dump(), ".json");
        CHECK_EQ(m.boxes.size(), std::size_t{1});
        CHECK_EQ(m.ignoredRotations, 1);
        if (!m.boxes.empty()) CHECK(m.boxes[0].rotated);
    }

    // =====================================================================
    // 6. Entrees degradees : jamais de plantage, jamais d'exception
    // =====================================================================
    {
        CHECK(parse("pas du json", ".json").empty());
        CHECK(parse("", ".json").empty());
        CHECK(parse("[]", ".json").empty());
        CHECK(parse("{}", ".json").empty());
        CHECK(parse("{\"elements\":\"texte\"}", ".json").empty());
        CHECK(parse("{\"elements\":[1,2,3]}", ".json").empty());
        CHECK(parse("{\"elements\":[{\"from\":[1,2],\"to\":[3,4,5]}]}", ".json")
                  .empty());
        CHECK(parse("{\"elements\":[{\"from\":[\"a\",\"b\",\"c\"],"
                    "\"to\":[1,2,3]}]}",
                    ".json")
                  .empty());
        CHECK(parse("{\"minecraft:geometry\":{}}", ".geo.json").empty());
    }

    // =====================================================================
    // 7. Bornes
    // =====================================================================
    {
        const json j = {
            {"elements",
             json::array({json{{"from", json::array({-2, 0, 1})},
                               {"to", json::array({2, 4, 3})}},
                          json{{"from", json::array({10, -6, 0})},
                               {"to", json::array({12, -2, 2})}}})}};
        const auto m = parse(j.dump(), ".json");
        const auto b = bounds_of(m);
        CHECK(std::fabs(b.minX + 2.0f) < 1e-5f);
        CHECK(std::fabs(b.maxX - 12.0f) < 1e-5f);
        CHECK(std::fabs(b.minY + 6.0f) < 1e-5f);
        CHECK(std::fabs(b.maxY - 4.0f) < 1e-5f);
        CHECK(std::fabs(b.cx() - 5.0f) < 1e-5f);
        CHECK(std::fabs(b.size() - 14.0f) < 1e-5f);
        // Modele vide : taille minimale de 1, pas de division par zero
        CHECK(std::fabs(bounds_of(Model{}).size() - 1.0f) < 1e-5f);
    }

    // =====================================================================
    // 8. Mannequin de demonstration
    // =====================================================================
    {
        const auto m = mannequin();
        CHECK_EQ(m.format, std::string("defaut"));
        // CORRECTIF vs C# : ses « yeux » avaient une profondeur nulle, donc
        // etaient jetes par la regle des dimensions positives. Les 12 boites
        // doivent toutes exister ici.
        CHECK_EQ(m.boxes.size(), std::size_t{12});
        for (const auto& b : m.boxes) {
            CHECK(b.w > 0.0f);
            CHECK(b.h > 0.0f);
            CHECK(b.d > 0.0f);
        }
        const auto bb = bounds_of(m);
        CHECK(bb.maxY > bb.minY);
    }

    // =====================================================================
    // 9. Lecture de fichier
    // =====================================================================
    {
        const fs::path dir = fs::temp_directory_path() / "tl-model3d-test";
        std::error_code ec;
        fs::remove_all(dir, ec);
        fs::create_directories(dir);

        const fs::path good = dir / "cube.json";
        {
            std::ofstream out(good, std::ios::binary);
            out << "{\"elements\":[{\"from\":[0,0,0],\"to\":[16,16,16]}]}";
        }
        auto m = load(good);
        CHECK(m.has_value());
        if (m) {
            CHECK_EQ(m->boxes.size(), std::size_t{1});
            CHECK_EQ(m->name, std::string("cube.json"));
        }

        // Fichier inexistant : nullopt
        CHECK(!load(dir / "absent.json").has_value());
        // Dossier : nullopt
        CHECK(!load(dir).has_value());
        // Fichier present mais inexploitable : modele VIDE, pas nullopt —
        // l'appelant doit pouvoir distinguer les deux cas.
        const fs::path bad = dir / "vide.json";
        {
            std::ofstream out(bad, std::ios::binary);
            out << "ceci n'est pas un modele";
        }
        auto e = load(bad);
        CHECK(e.has_value());
        if (e) CHECK(e->empty());

        fs::remove_all(dir, ec);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

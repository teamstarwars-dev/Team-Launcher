// Tests du developpement de mods : normalisations, generation des fichiers de
// projet, ecriture sur disque, detection de la chaine d'outils, execution
// d'une commande. Aucun appel reseau (resolve_deps n'est pas exerce ici).

#include "moddev.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace tl::moddev;
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

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

int main() {
    // =====================================================================
    // 1. Identifiant de mod
    // =====================================================================
    CHECK_EQ(mod_id_from("Mon Mod"), std::string("mon_mod"));
    CHECK_EQ(mod_id_from("Super-Mod 2000"), std::string("super_mod_2000"));
    // CORRECTIF vs C# : les accents doivent disparaitre, pas casser l'id.
    CHECK_EQ(mod_id_from("Épée Enchantée"), std::string("epee_enchantee"));
    CHECK_EQ(mod_id_from("  espaces  "), std::string("espaces"));
    // Fabric exige une premiere lettre alphabetique
    CHECK_EQ(mod_id_from("2000 Blocs"), std::string("blocs"));
    CHECK_EQ(mod_id_from("!!!"), std::string(""));
    CHECK_EQ(mod_id_from(""), std::string(""));
    CHECK(mod_id_from(std::string(200, 'a')).size() <= 63);

    // =====================================================================
    // 2. Nom de classe Java
    // =====================================================================
    CHECK_EQ(class_name_from("Mon Mod"), std::string("MonMod"));
    CHECK_EQ(class_name_from("super-mod 2000"), std::string("SuperMod2000"));
    CHECK_EQ(class_name_from("Épée"), std::string("Epee"));
    CHECK_EQ(class_name_from("3D Tools"), std::string("DTools"));
    CHECK_EQ(class_name_from("???"), std::string("MonMod"));

    // =====================================================================
    // 3. Paquet Java
    // =====================================================================
    CHECK(valid_package("com.exemple.monmod"));
    CHECK(valid_package("fr"));
    CHECK(valid_package("a.b2.c_d"));
    CHECK(!valid_package(""));
    CHECK(!valid_package("com..exemple"));
    CHECK(!valid_package("com.exemple."));
    CHECK(!valid_package("Com.Exemple"));   // majuscules
    CHECK(!valid_package("com.2exemple"));  // chiffre en tete
    CHECK(!valid_package("com.exemple-mod"));
    CHECK(!valid_package("com.class.mod")); // mot reserve
    CHECK(!valid_package("com.new"));

    // =====================================================================
    // 4. UUID v4
    // =====================================================================
    {
        std::set<std::string> seen;
        for (int i = 0; i < 200; ++i) {
            const std::string u = uuid_v4();
            CHECK_EQ(u.size(), std::size_t{36});
            CHECK(u[8] == '-' && u[13] == '-' && u[18] == '-' && u[23] == '-');
            CHECK(u[14] == '4');                      // version
            CHECK(u[19] == '8' || u[19] == '9' || u[19] == 'a' || u[19] == 'b');
            seen.insert(u);
        }
        CHECK_EQ(seen.size(), std::size_t{200}); // aucune collision
    }

    // =====================================================================
    // 5. Fichiers generes — Fabric
    // =====================================================================
    Deps deps;
    deps.yarn = "1.21.4+build.8";
    deps.loader = "0.16.9";
    deps.fabricApi = "0.114.0+1.21.4";
    deps.resolved = true;
    {
        ProjectSpec s;
        s.loader = Loader::Fabric;
        s.mcVersion = "1.21.4";
        s.name = "Épée Enchantée";
        s.pkg = "fr.exemple.epee";
        const auto f = project_files(s, deps);

        CHECK(f.count("build.gradle"));
        CHECK(f.count("settings.gradle"));
        CHECK(f.count("gradle.properties"));
        CHECK(f.count(".gitignore"));
        CHECK(f.count("README.md"));
        CHECK(f.count("src/main/resources/fabric.mod.json"));
        // Le chemin de la classe suit le paquet, et le nom est desaccentue.
        CHECK(f.count("src/main/java/fr/exemple/epee/EpeeEnchantee.java"));

        const auto& props = f.at("gradle.properties");
        CHECK(props.find("yarn_mappings=1.21.4+build.8") != std::string::npos);
        CHECK(props.find("loader_version=0.16.9") != std::string::npos);
        CHECK(props.find("minecraft_version=1.21.4") != std::string::npos);
        CHECK(props.find("maven_group=fr.exemple.epee") != std::string::npos);

        // Le manifeste Fabric doit etre du JSON valide, avec le bon id.
        const auto j = json::parse(f.at("src/main/resources/fabric.mod.json"),
                                   nullptr, false);
        CHECK(!j.is_discarded());
        if (!j.is_discarded()) {
            CHECK_EQ(j.value("id", ""), std::string("epee_enchantee"));
            CHECK_EQ(j["entrypoints"]["main"][0].get<std::string>(),
                     std::string("fr.exemple.epee.EpeeEnchantee"));
        }

        const auto& src = f.at("src/main/java/fr/exemple/epee/EpeeEnchantee.java");
        CHECK(src.rfind("package fr.exemple.epee;", 0) == 0);
        CHECK(src.find("class EpeeEnchantee implements ModInitializer") !=
              std::string::npos);
        CHECK(src.find("\"epee_enchantee\"") != std::string::npos);
    }

    // =====================================================================
    // 6. Forge et NeoForge : le bon manifeste, la bonne annotation
    // =====================================================================
    {
        ProjectSpec s;
        s.mcVersion = "1.21.1";
        s.name = "Test Mod";
        s.pkg = "com.exemple.test";

        s.loader = Loader::Forge;
        auto f = project_files(s, deps);
        CHECK(f.count("src/main/resources/META-INF/mods.toml"));
        CHECK(!f.count("src/main/resources/META-INF/neoforge.mods.toml"));
        CHECK(!f.count("src/main/resources/fabric.mod.json"));
        CHECK(f.at("src/main/java/com/exemple/test/TestMod.java")
                  .find("net.minecraftforge.fml.common.Mod") != std::string::npos);

        s.loader = Loader::NeoForge;
        f = project_files(s, deps);
        CHECK(f.count("src/main/resources/META-INF/neoforge.mods.toml"));
        CHECK(!f.count("src/main/resources/META-INF/mods.toml"));
        CHECK(f.at("src/main/java/com/exemple/test/TestMod.java")
                  .find("net.neoforged.fml.common.Mod") != std::string::npos);
    }

    // =====================================================================
    // 7. Bedrock : deux manifestes JSON valides, UUID tous distincts
    // =====================================================================
    {
        ProjectSpec s;
        s.loader = Loader::Bedrock;
        s.mcVersion = "1.21.50";
        s.name = "Mon Addon";
        const auto f = project_files(s, deps);
        CHECK(f.count("BP/manifest.json"));
        CHECK(f.count("RP/manifest.json"));
        CHECK(!f.count("build.gradle")); // pas de Gradle pour Bedrock

        std::set<std::string> uuids;
        for (const char* k : {"BP/manifest.json", "RP/manifest.json"}) {
            const auto j = json::parse(f.at(k), nullptr, false);
            CHECK(!j.is_discarded());
            if (j.is_discarded()) continue;
            CHECK_EQ(j.value("format_version", 0), 2);
            uuids.insert(j["header"].value("uuid", ""));
            uuids.insert(j["modules"][0].value("uuid", ""));
        }
        // 4 UUID, tous differents (le C# en generait 4 aussi, mais rien ne
        // le verifiait).
        CHECK_EQ(uuids.size(), std::size_t{4});
        // Les dossiers vides du C# sont materialises par un .gitkeep.
        CHECK(f.count("BP/scripts/.gitkeep"));
        CHECK(f.count("RP/textures/.gitkeep"));
    }

    // =====================================================================
    // 8. Ecriture sur disque, et refus d'ecraser sans le demander
    // =====================================================================
    {
        const fs::path dir = fs::temp_directory_path() / "tl-moddev-test";
        std::error_code ec;
        fs::remove_all(dir, ec);

        ProjectSpec s;
        s.loader = Loader::Fabric;
        s.mcVersion = "1.21.4";
        s.name = "Mon Mod";
        s.pkg = "com.exemple.monmod";
        s.dir = dir;

        std::vector<std::string> logs;
        auto r = create_project(s, deps, false,
                                [&](const std::string& m) { logs.push_back(m); });
        CHECK(r.ok);
        CHECK(r.error.empty());
        CHECK(!r.written.empty());
        CHECK(r.kept.empty());
        CHECK(!logs.empty());
        CHECK(fs::exists(dir / "build.gradle"));
        CHECK(fs::exists(dir / "src/main/java/com/exemple/monmod/MonMod.java"));

        // Modification manuelle, puis regeneration SANS overwrite : conservee.
        {
            // Binaire : en mode texte Windows traduirait la fin de ligne, et
            // la comparaison avec la relecture binaire echouerait.
            std::ofstream out(dir / "build.gradle",
                              std::ios::binary | std::ios::trunc);
            out << "// mes reglages a moi\n";
        }
        auto r2 = create_project(s, deps, false, nullptr);
        CHECK(r2.ok);
        CHECK(!r2.kept.empty());
        CHECK(r2.written.empty()); // tout etait deja la
        CHECK_EQ(read_file(dir / "build.gradle"),
                 std::string("// mes reglages a moi\n"));

        // Avec overwrite : remplace.
        auto r3 = create_project(s, deps, true, nullptr);
        CHECK(r3.ok);
        CHECK(r3.kept.empty());
        CHECK(read_file(dir / "build.gradle").find("fabric-loom") !=
              std::string::npos);

        // Refus : nom vide, dossier vide, paquet invalide
        ProjectSpec bad = s;
        bad.name.clear();
        CHECK(!create_project(bad, deps, false, nullptr).error.empty());
        bad = s;
        bad.dir.clear();
        CHECK(!create_project(bad, deps, false, nullptr).error.empty());
        bad = s;
        bad.pkg = "Com.Majuscule";
        CHECK(!create_project(bad, deps, false, nullptr).error.empty());
        bad = s;
        bad.name = "!!!"; // aucun identifiant exploitable
        CHECK(!create_project(bad, deps, false, nullptr).error.empty());

        // =================================================================
        // 9. Chaine d'outils : un projet sans wrapper doit le DIRE
        // =================================================================
        {
            const auto t = detect_toolchain(dir);
            // CORRECTIF vs C# : create_project ne produit pas de wrapper, donc
            // la detection doit soit trouver gradle dans le PATH, soit
            // expliquer ce qui manque — jamais « cree le projet d'abord ».
            CHECK(!t.wrapper);
            if (t.command.empty()) CHECK(!t.problem.empty());
            if (!t.jdk) CHECK(t.problem.find("JDK") != std::string::npos);
            std::printf("INFO chaine : wrapper=%d gradle=%d jdk=%d cmd=%s\n",
                        t.wrapper ? 1 : 0, t.gradleOnPath ? 1 : 0, t.jdk ? 1 : 0,
                        t.command.empty() ? "(aucune)" : t.command.c_str());
        }

        // =================================================================
        // 10. Execution d'une commande : lignes remontees, code de sortie
        // =================================================================
        {
            std::vector<std::string> out;
            const int code = run(dir, "cmd.exe /c echo ligne1&echo ligne2",
                                 [&](const std::string& l) { out.push_back(l); });
            CHECK_EQ(code, 0);
            CHECK_EQ(out.size(), std::size_t{2});
            if (out.size() == 2) {
                CHECK_EQ(out[0], std::string("ligne1"));
                CHECK_EQ(out[1], std::string("ligne2"));
            }

            // Code de sortie non nul remonte tel quel
            CHECK_EQ(run(dir, "cmd.exe /c exit 3", nullptr), 3);
            // Executable introuvable : -1, jamais un plantage
            CHECK_EQ(run(dir, "tl-inexistant-xyz.exe", nullptr), -1);
            CHECK_EQ(run(dir, "", nullptr), -1);
        }

        fs::remove_all(dir, ec);
    }

    // =====================================================================
    // 11. Chargeurs et listes de versions
    // =====================================================================
    CHECK_EQ(std::string(loader_name(loader_from("neoforge"))),
             std::string("NeoForge"));
    CHECK_EQ(std::string(loader_name(loader_from("FORGE"))), std::string("Forge"));
    CHECK_EQ(std::string(loader_name(loader_from("n'importe quoi"))),
             std::string("Fabric"));
    for (auto l : {Loader::Fabric, Loader::Forge, Loader::NeoForge,
                   Loader::Bedrock})
        CHECK(!versions_for(l).empty());

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

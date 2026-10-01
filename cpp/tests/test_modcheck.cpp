// Phase 5 : lecture des metadonnees de mods et detection de conflits.
//
// Les analyseurs et la comparaison de versions sont purs : ils se testent
// sans fabriquer d'archive. Un seul cas construit un vrai .jar, pour
// verifier que la lecture d'archive et l'ordre de priorite des manifestes
// tiennent aussi.

#include "modcheck.hpp"
#include "modmeta.hpp"
#include "util_zip.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace tl;
using namespace tl::modmeta;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const auto _a = (a);                                                 \
        const auto _b = (b);                                                 \
        if (!(_a == _b)) {                                                   \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a,    \
                        #b);                                                 \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Un mod deja analyse, pour nourrir modcheck sans passer par un .jar.
static Mod mk(const std::string& file, const std::string& id, Loader l,
              const std::string& ver = "1.0",
              const std::string& mcRange = "") {
    Mod m;
    m.file = file;
    m.id = id;
    m.name = id;
    m.version = ver;
    m.loader = l;
    m.mcRange = mcRange;
    return m;
}

static int count_sev(const modcheck::Report& r, modcheck::Severity s) {
    int n = 0;
    for (const auto& i : r.issues)
        if (i.sev == s) ++n;
    return n;
}

static bool has_title(const modcheck::Report& r, const std::string& needle) {
    for (const auto& i : r.issues)
        if (i.title.find(needle) != std::string::npos) return true;
    return false;
}

int main() {
    // =====================================================================
    // 1. Comparaison de versions
    // =====================================================================
    CHECK(compare_versions("1.20.1", "1.20.1") == 0);
    CHECK(compare_versions("1.20.2", "1.20.1") > 0);
    CHECK(compare_versions("1.20", "1.20.0") == 0);   // 1.20 == 1.20.0
    CHECK(compare_versions("1.9", "1.10") < 0);       // numerique, pas texte
    CHECK(compare_versions("0.5.8", "0.5.11") < 0);
    // Semver : une preversion PRECEDE la version finale.
    CHECK(compare_versions("1.0", "1.0-beta1") > 0);
    CHECK(compare_versions("1.0-alpha", "1.0-beta") < 0);
    // Une suite de chiffres absurde ne doit pas deborder ni inverser l'ordre.
    CHECK(compare_versions("1.99999999999999999999", "2.0") < 0);

    // =====================================================================
    // 2. Contraintes de version
    // =====================================================================
    // Vide et jokers
    CHECK(version_matches("1.20.1", ""));
    CHECK(version_matches("1.20.1", "*"));
    // Egalite (lecture Fabric d'une version nue)
    CHECK(version_matches("1.20.1", "1.20.1"));
    CHECK(!version_matches("1.20.2", "1.20.1"));
    // Operateurs
    CHECK(version_matches("1.20.1", ">=1.20"));
    CHECK(!version_matches("1.19.4", ">=1.20"));
    CHECK(version_matches("1.20.1", "<1.21"));
    CHECK(version_matches("1.20.1", ">=1.20 <1.21"));
    CHECK(!version_matches("1.21", ">=1.20 <1.21"));
    // Operateur separe de son operande par une espace
    CHECK(version_matches("1.20.1", ">= 1.20"));
    // Intervalles Maven (Forge)
    CHECK(version_matches("1.20.1", "[1.20,1.21)"));
    CHECK(!version_matches("1.21", "[1.20,1.21)"));
    CHECK(version_matches("1.21", "[1.20,1.21]"));
    CHECK(!version_matches("1.20", "(1.20,1.21)"));
    CHECK(version_matches("47.2.0", "[47,)"));       // borne haute vide
    CHECK(version_matches("1.20.1", "[1.20.1]"));    // ponctuel
    CHECK(!version_matches("1.20.2", "[1.20.1]"));
    // La virgule d'un intervalle n'est PAS un separateur de contraintes
    CHECK(version_matches("1.20.1", "[1.20,1.21)"));
    // Alternatives
    CHECK(version_matches("1.19.2", "1.20.1 || 1.19.2"));
    CHECK(!version_matches("1.18", "1.20.1 || 1.19.2"));
    // ~ et ^
    CHECK(version_matches("1.20.5", "~1.20.1"));
    CHECK(!version_matches("1.21.0", "~1.20.1"));
    CHECK(version_matches("1.99", "^1.20"));
    CHECK(!version_matches("2.0", "^1.20"));
    // Joker de branche
    CHECK(version_matches("1.20.4", "1.20.x"));
    CHECK(!version_matches("1.19.4", "1.20.x"));
    // Syntaxe exotique : on accepte plutot que de bloquer a tort
    CHECK(version_matches("1.20.1", "{}{}{}"));

    // =====================================================================
    // 3. fabric.mod.json
    // =====================================================================
    {
        const auto m = parse_fabric(R"({
            "schemaVersion": 1,
            "id": "sodium",
            "name": "Sodium",
            "version": "0.5.8",
            "depends": {
                "minecraft": ">=1.20.1 <1.21",
                "java": ">=17",
                "fabricloader": ">=0.14.0",
                "fabric-api": "*"
            },
            "suggests": { "iris": "*" }
        })");
        CHECK_EQ(m.id, std::string("sodium"));
        CHECK_EQ(m.name, std::string("Sodium"));
        CHECK_EQ(m.version, std::string("0.5.8"));
        CHECK(m.loader == Loader::Fabric);
        CHECK_EQ(m.mcRange, std::string(">=1.20.1 <1.21"));
        CHECK(m.readError.empty());
        // minecraft et java ne doivent PAS figurer comme dependances : les
        // chercher dans le dossier mods produirait de faux manquants.
        bool hasFabricApi = false, hasIris = false, hasJava = false;
        for (const auto& d : m.deps) {
            if (d.id == "java" || d.id == "minecraft") hasJava = true;
            if (d.id == "fabric-api") { hasFabricApi = true; CHECK(d.mandatory); }
            if (d.id == "iris") { hasIris = true; CHECK(!d.mandatory); }
        }
        CHECK(hasFabricApi);
        CHECK(hasIris);
        CHECK(!hasJava);
    }
    // Contrainte donnee sous forme de tableau (alternatives)
    {
        const auto m = parse_fabric(
            R"({"id":"x","version":"1","depends":{"minecraft":["1.20.1","1.19.4"]}})");
        CHECK(version_matches("1.19.4", m.mcRange));
        CHECK(!version_matches("1.18", m.mcRange));
    }
    // JSON casse : erreur rapportee, pas d'exception
    {
        const auto m = parse_fabric("{ pas du json");
        CHECK(!m.readError.empty());
        CHECK(m.id.empty());
    }

    // =====================================================================
    // 4. mods.toml (Forge / NeoForge)
    // =====================================================================
    {
        // Delimiteur TOML( ) : le contenu comporte `)"` (dans
        // versionRange="[47,)"), qui fermerait une chaine brute ordinaire.
        const auto m = parse_mods_toml(R"TOML(
modLoader="javafml"
loaderVersion="[47,)"
license="MIT"

[[mods]]
modId="jei"
version="15.2.0.27"
displayName="Just Enough Items"   # commentaire ignore

[[dependencies.jei]]
    modId="forge"
    mandatory=true
    versionRange="[47,)"
[[dependencies.jei]]
    modId="minecraft"
    mandatory=true
    versionRange="[1.20.1,1.21)"
[[dependencies.jei]]
    modId="patchouli"
    mandatory=false
    versionRange="*"
)TOML",
                                       false);
        CHECK_EQ(m.id, std::string("jei"));
        CHECK_EQ(m.name, std::string("Just Enough Items"));
        CHECK_EQ(m.version, std::string("15.2.0.27"));
        CHECK(m.loader == Loader::Forge);
        CHECK_EQ(m.mcRange, std::string("[1.20.1,1.21)"));
        CHECK(m.readError.empty());
        // forge et minecraft ecartes, patchouli garde en facultatif
        CHECK_EQ(m.deps.size(), size_t{1});
        if (m.deps.size() == 1) {
            CHECK_EQ(m.deps[0].id, std::string("patchouli"));
            CHECK(!m.deps[0].mandatory);
        }
    }
    // NeoForge 1.21 : `type = "required"` remplace `mandatory`
    {
        const auto m = parse_mods_toml(R"TOML(
[[mods]]
modId="test"
version="1.0"
[[dependencies.test]]
    modId="curios"
    type="optional"
    versionRange="[5,)"
)TOML",
                                       true);
        CHECK(m.loader == Loader::NeoForge);
        CHECK_EQ(m.deps.size(), size_t{1});
        if (m.deps.size() == 1) CHECK(!m.deps[0].mandatory);
    }
    // Un « # » dans une chaine n'est pas un commentaire
    {
        const auto m = parse_mods_toml(
            "[[mods]]\nmodId=\"x\"\ndisplayName=\"Mod #1\"\n", false);
        CHECK_EQ(m.name, std::string("Mod #1"));
    }
    // Plusieurs [[mods]] : on retient le premier, pas un melange des deux
    {
        const auto m = parse_mods_toml(
            "[[mods]]\nmodId=\"premier\"\nversion=\"1\"\n"
            "[[mods]]\nmodId=\"second\"\nversion=\"2\"\n",
            false);
        CHECK_EQ(m.id, std::string("premier"));
        CHECK_EQ(m.version, std::string("1"));
    }

    // =====================================================================
    // 5. quilt.mod.json et mcmod.info
    // =====================================================================
    {
        const auto m = parse_quilt(R"({
          "schema_version": 1,
          "quilt_loader": {
            "id": "exemple", "version": "2.0",
            "metadata": { "name": "Exemple" },
            "depends": [
              { "id": "minecraft", "versions": ">=1.20" },
              { "id": "qsl", "versions": "*" },
              { "id": "extra", "versions": "*", "optional": true }
            ]
          }
        })");
        CHECK_EQ(m.id, std::string("exemple"));
        CHECK_EQ(m.name, std::string("Exemple"));
        CHECK(m.loader == Loader::Quilt);
        CHECK_EQ(m.mcRange, std::string(">=1.20"));
        CHECK_EQ(m.deps.size(), size_t{2});
    }
    {
        const auto m = parse_mcmod_info(
            R"J([{"modid":"vieux","name":"Vieux mod","version":"1.2",)J"
            R"J("mcversion":"1.12.2","requiredMods":["autre@[1.0,)"]}])J");
        CHECK_EQ(m.id, std::string("vieux"));
        CHECK_EQ(m.mcRange, std::string("1.12.2"));
        CHECK_EQ(m.deps.size(), size_t{1});
        if (m.deps.size() == 1) CHECK_EQ(m.deps[0].id, std::string("autre"));
    }

    // =====================================================================
    // 6. Analyse : chaque conflit isolement
    // =====================================================================
    // Instance saine
    {
        const auto r = modcheck::analyse(
            {mk("a.jar", "a", Loader::Fabric, "1.0", ">=1.20 <1.21"),
             mk("b.jar", "b", Loader::Fabric, "1.0", "1.20.1")},
            "Fabric", "1.20.1");
        CHECK_EQ(r.errors, 0);
        CHECK_EQ(r.warnings, 0);
        CHECK_EQ(r.analysed, 2);
    }
    // Mauvais chargeur
    {
        const auto r = modcheck::analyse(
            {mk("forge.jar", "f", Loader::Forge)}, "Fabric", "1.20.1");
        CHECK_EQ(r.errors, 1);
        CHECK(has_title(r, "Prévu pour Forge"));
    }
    // Quilt accepte les mods Fabric : ce n'est pas un conflit
    {
        const auto r = modcheck::analyse(
            {mk("fab.jar", "f", Loader::Fabric)}, "Quilt", "1.20.1");
        CHECK_EQ(r.errors, 0);
    }
    // Forge vs NeoForge : suspect, pas bloquant
    {
        const auto r = modcheck::analyse(
            {mk("f.jar", "f", Loader::Forge)}, "NeoForge", "1.21");
        CHECK_EQ(r.errors, 0);
        CHECK_EQ(r.warnings, 1);
    }
    // Mauvaise version de Minecraft
    {
        const auto r = modcheck::analyse(
            {mk("v.jar", "v", Loader::Fabric, "1.0", "[1.19,1.20)")}, "Fabric",
            "1.20.1");
        CHECK_EQ(r.errors, 1);
        CHECK(has_title(r, "Version de Minecraft"));
    }
    // Dependance obligatoire manquante
    {
        Mod m = mk("m.jar", "m", Loader::Fabric);
        m.deps.push_back(Dep{"fabric-api", true, "*"});
        const auto r = modcheck::analyse({m}, "Fabric", "1.20.1");
        CHECK_EQ(r.errors, 1);
        CHECK(has_title(r, "Dépendance manquante : fabric-api"));
    }
    // Dependance presente mais desactivee : toujours manquante
    {
        Mod m = mk("m.jar", "m", Loader::Fabric);
        m.deps.push_back(Dep{"api", true, "*"});
        Mod dep = mk("api.jar.disabled", "api", Loader::Fabric);
        dep.disabled = true;
        const auto r = modcheck::analyse({m, dep}, "Fabric", "1.20.1");
        CHECK_EQ(r.errors, 1);
        CHECK_EQ(r.skipped, 1);
        CHECK_EQ(r.analysed, 1);
    }
    // Dependance presente dans la mauvaise version : avertissement
    {
        Mod m = mk("m.jar", "m", Loader::Fabric);
        m.deps.push_back(Dep{"api", true, ">=2.0"});
        const auto r = modcheck::analyse(
            {m, mk("api.jar", "api", Loader::Fabric, "1.5")}, "Fabric",
            "1.20.1");
        CHECK_EQ(r.errors, 0);
        CHECK_EQ(r.warnings, 1);
    }
    // Dependance facultative absente : suggestion, pas probleme
    {
        Mod m = mk("m.jar", "m", Loader::Fabric);
        m.deps.push_back(Dep{"iris", false, "*"});
        const auto r = modcheck::analyse({m}, "Fabric", "1.20.1");
        CHECK_EQ(r.errors, 0);
        CHECK_EQ(r.warnings, 0);
        CHECK_EQ(r.suggestions.size(), size_t{1});
        if (!r.suggestions.empty())
            CHECK_EQ(r.suggestions[0], std::string("iris"));
    }
    // Doublon de modId
    {
        const auto r = modcheck::analyse(
            {mk("jei-1.jar", "jei", Loader::Forge, "15.0"),
             mk("jei-2.jar", "jei", Loader::Forge, "15.2")},
            "Forge", "1.20.1");
        CHECK(has_title(r, "double"));
        CHECK(r.errors >= 1);
    }
    // Instance sans chargeur : UN message, pas un par mod
    {
        const auto r = modcheck::analyse(
            {mk("a.jar", "a", Loader::Fabric), mk("b.jar", "b", Loader::Forge),
             mk("c.jar", "c", Loader::Forge)},
            "Vanilla", "1.20.1");
        CHECK_EQ(r.errors, 1);
        CHECK(has_title(r, "Aucun chargeur"));
    }
    // Vanilla sans mods : rien a dire
    {
        const auto r = modcheck::analyse({}, "Vanilla", "1.20.1");
        CHECK_EQ(r.errors, 0);
        CHECK_EQ(r.issues.size(), size_t{0});
    }
    // Le plus grave est presente en premier
    {
        Mod warn = mk("w.jar", "w", Loader::Fabric);
        warn.readError = "archive tronquee";
        Mod err = mk("e.jar", "e", Loader::Forge);
        const auto r = modcheck::analyse({warn, err}, "Fabric", "1.20.1");
        CHECK(r.issues.size() >= 2);
        if (r.issues.size() >= 2)
            CHECK(r.issues[0].sev == modcheck::Severity::Error);
        CHECK_EQ(count_sev(r, modcheck::Severity::Warning), 1);
    }

    // =====================================================================
    // 7. Vraie archive : lecture et priorite des manifestes
    // =====================================================================
    {
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path() / "tl_test_modmeta";
        fs::remove_all(tmp, ec);
        const fs::path src = tmp / "jarsrc";
        fs::create_directories(src / "META-INF", ec);

        // Un jar multi-chargeurs : deux manifestes dans la meme archive.
        // NeoForge doit l'emporter sur Fabric — c'est celui qui decrit le
        // plus precisement ce que le jar sait faire.
        std::ofstream(src / "fabric.mod.json")
            << R"({"id":"double","version":"1.0","depends":{"minecraft":"1.20.1"}})";
        std::ofstream(src / "META-INF" / "neoforge.mods.toml")
            << "[[mods]]\nmodId=\"double\"\nversion=\"2.0\"\n";
        const fs::path jar = tmp / "double.jar";
        CHECK(zip_create_from_dir(src, jar));

        const auto m = read_jar(jar);
        CHECK_EQ(m.file, std::string("double.jar"));
        CHECK(!m.disabled);
        CHECK(m.loader == Loader::NeoForge);
        CHECK_EQ(m.version, std::string("2.0"));

        // Un fichier qui n'est pas une archive : signale, pas d'exception.
        const fs::path bad = tmp / "pasunjar.jar";
        std::ofstream(bad) << "ceci n'est pas un zip";
        const auto b = read_jar(bad);
        CHECK(!b.readError.empty());
        CHECK_EQ(b.name, std::string("pasunjar.jar")); // retombe sur le fichier

        // read_dir voit les deux, et marque le desactive.
        fs::copy_file(jar, tmp / "off.jar.disabled", ec);
        const auto all = read_dir(tmp);
        CHECK_EQ(all.size(), size_t{3});
        bool sawDisabled = false;
        for (const auto& x : all)
            if (x.file == "off.jar.disabled") {
                sawDisabled = true;
                CHECK(x.disabled);
            }
        CHECK(sawDisabled);

        // Dossier absent : liste vide, pas d'erreur.
        CHECK(read_dir(tmp / "nexistepas").empty());

        fs::remove_all(tmp, ec);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

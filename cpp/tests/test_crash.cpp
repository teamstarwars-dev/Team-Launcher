// Diagnostic de crash v2 : désignation du mod coupable.
//
// Les cas viennent de vrais rapports. Le plus instructif est celui qui a
// motivé cette version : une NullPointerException levée par un mod, dans
// un rapport dont la section « System Details » contient le mot OpenGL —
// que la v1 diagnostiquait « problème de pilote graphique ».
//
// TL_CRASH_DIR=<dossier> passe en plus tous les .txt d'un dossier réel,
// en affichant le verdict : utile pour éprouver l'analyseur sur des
// rapports qu'on n'a pas écrits soi-même.

#include "crash_analyzer.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using namespace tl::crash;

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

// Rapport Forge 1.12 abrégé, fidèle au format réel : un mod lève une NPE,
// et la section System Details contient « OpenGL ».
static const char* kForge112 = R"CRASH(---- Minecraft Crash Report ----

// Uh... Did I do that?

Time: 9/1/26 5:45 PM
Description: Initializing game

java.lang.NullPointerException: Can't use a null-name for the registry, object null.
	at com.google.common.base.Preconditions.checkNotNull(Preconditions.java:864)
	at net.minecraftforge.registries.ForgeRegistry.add(ForgeRegistry.java:287)
	at fr.webscreen.registry.ModItems.registerItems(ModItems.java:35)
	at net.minecraftforge.fml.common.eventhandler.EventBus.post(EventBus.java:182)
	at net.minecraft.client.main.Main.main(SourceFile:123)

-- System Details --
Details:
	Minecraft Version: 1.12.2
	Operating System: Windows 10 (amd64) version 10.0
	Java Version: 1.8.0_312, Temurin
	OpenGL: NVIDIA GeForce RTX 3060/PCIe/SSE2 GL version 4.6.0
	FML: MCP 9.42 Powered by Forge 14.23.5.2859 24 mods loaded, 24 mods active
	States: 'U' = Unloaded 'L' = Loaded 'C' = Constructed

	| State | ID                | Version               | Source                                    | Signature |
	|:----- |:----------------- |:--------------------- |:----------------------------------------- |:--------- |
	| LCH   | minecraft         | 1.12.2                | minecraft.jar                             | None      |
	| LCH   | forge             | 14.23.5.2859          | forge-1.12.2-14.23.5.2859.jar             | e3c3d50c  |
	| LCH   | chiselsandbits    | 14.33                 | chiselsandbits-14.33.jar                  | None      |
	| LCH   | webscreen         | 2.0.0                 | WebDisplay2-2.0.0.jar                     | None      |
	| LCH   | securitycraft     | v1.10.1               | [1.12.2] SecurityCraft v1.10.1.jar        | None      |
)CRASH";

// Forge moderne : « Mod List: » et une ligne « Suspected Mods: ».
static const char* kForgeModern = R"CRASH(---- Minecraft Crash Report ----
Description: Mod loading error has occurred

java.lang.RuntimeException: Attempted to load class net/minecraft/client for invalid dist
	at mezz.jei.library.load.PluginLoader.<init>(PluginLoader.java:51)
	at net.minecraftforge.fml.ModContainer.acceptEvent(ModContainer.java:115)

-- System Details --
Details:
	Minecraft Version: 1.20.1
	Suspected Mods: Just Enough Items (jei)
	Mod List:
		client-1.20.1-20230612.114412-srg.jar             |Minecraft                     |minecraft                     |1.20.1              |COMMON_SET|Manifest
		jei-1.20.1-forge-15.2.0.27.jar                    |Just Enough Items             |jei                           |15.2.0.27           |COMMON_SET|Manifest
		sodium-fabric-0.5.8.jar                           |Sodium                        |sodium                        |0.5.8               |COMMON_SET|Manifest
)CRASH";

// Fabric : section « Fabric Mods: ».
static const char* kFabric = R"CRASH(---- Minecraft Crash Report ----
Description: Rendering overlay

java.lang.IllegalStateException: Rendering pipeline not ready
	at me.jellysquid.mods.sodium.client.render.SodiumWorldRenderer.render(SodiumWorldRenderer.java:210)
	at net.minecraft.class_757.method_3188(class_757.java:1050)

-- System Details --
Details:
	Minecraft Version: 1.20.1
	Fabric Mods:
		fabricloader: Fabric Loader 0.14.24
		sodium: Sodium 0.5.8
		iris: Iris 1.6.9
)CRASH";

// Mémoire : la cause est l'environnement, pas le mod cité dans la pile.
static const char* kOom = R"CRASH(---- Minecraft Crash Report ----
Description: Ticking entity

java.lang.OutOfMemoryError: Java heap space
	at fr.webscreen.registry.ModItems.registerItems(ModItems.java:35)

-- System Details --
Details:
	Minecraft Version: 1.12.2
	| State | ID        | Version | Source                | Signature |
	| LCH   | webscreen | 2.0.0   | WebDisplay2-2.0.0.jar | None      |
)CRASH";

// Cas réel, et le plus instructif de tous : l'exception VISIBLE est un
// NoClassDefFoundError sur une classe d'OptiFine, mais la vraie cause,
// trente lignes plus bas, est un manque de mémoire — la classe n'a pas pu
// être chargée faute de place. Diagnostiquer « OptiFine est cassé »
// enverrait l'utilisateur désinstaller un mod innocent.
static const char* kOomCascade = R"CRASH(---- Minecraft Crash Report ----
Description: Unexpected error

java.lang.NoClassDefFoundError: net/optifine/entity/model/CustomModelRegistry
	at net.optifine.entity.model.CustomEntityModels.getModelLocations(CustomEntityModels.java:145)
	at net.optifine.util.TextureUtils.resourcesReloaded(TextureUtils.java:319)
	at net.minecraft.client.renderer.EntityRenderer.frameInit(EntityRenderer.java:2818)
Caused by: java.lang.OutOfMemoryError: GC overhead limit exceeded

-- System Details --
Details:
	Minecraft Version: 1.12.2
	OpenGL: NVIDIA GeForce RTX 3060 GL version 4.6.0
	| State | ID        | Version | Source                | Signature |
	| LCH   | optifine  | 1.12.2  | OptiFine_1.12.2.jar   | None      |
)CRASH";

int main() {
    // =====================================================================
    // 1. Forge 1.12 : le mod est nommé, et ce n'est PAS le pilote graphique
    // =====================================================================
    {
        const Report r = analyze_report(kForge112);
        CHECK(r.found);
        // Le défaut que cette version corrige : « OpenGL » apparaît dans
        // les détails système de tous les rapports.
        CHECK(r.cause != Cause::Graphics);
        CHECK_EQ(r.loader, std::string("Forge"));
        CHECK_EQ(r.mcVersion, std::string("1.12.2"));
        CHECK(!r.suspects.empty());
        if (!r.suspects.empty()) {
            // Le nom du jar ne contient pas « webscreen » : seule la
            // correspondance identifiant ↔ paquet Java pouvait le trouver.
            CHECK_EQ(r.suspects[0].modId, std::string("webscreen"));
            CHECK_EQ(r.suspects[0].source, std::string("WebDisplay2-2.0.0.jar"));
            CHECK_EQ(r.suspects[0].version, std::string("2.0.0"));
            CHECK(r.suspects[0].evidence.find("fr.webscreen") !=
                  std::string::npos);
            CHECK(r.suspects[0].score >= 50);
        }
        CHECK(r.summary.find("webscreen") != std::string::npos);
        CHECK(r.summary.find("WebDisplay2") != std::string::npos);
        // Les mods non impliqués ne doivent pas être accusés.
        for (const auto& s : r.suspects)
            CHECK(s.modId != "chiselsandbits" && s.modId != "securitycraft");
        std::printf("INFO forge 1.12 -> %s\n", r.summary.c_str());
    }

    // =====================================================================
    // 2. Forge moderne : « Mod List: » et « Suspected Mods: »
    // =====================================================================
    {
        const Report r = analyze_report(kForgeModern);
        CHECK(r.found);
        CHECK(!r.suspects.empty());
        if (!r.suspects.empty()) {
            CHECK_EQ(r.suspects[0].modId, std::string("jei"));
            // Désigné par le jeu lui-même : confiance maximale.
            CHECK_EQ(r.suspects[0].score, 100);
            CHECK(r.suspects[0].source.find("jei-1.20.1") != std::string::npos);
        }
        // Sodium est dans la liste mais pas dans la pile.
        for (const auto& s : r.suspects) CHECK(s.modId != "sodium");
    }

    // =====================================================================
    // 3. Fabric
    // =====================================================================
    {
        const Report r = analyze_report(kFabric);
        CHECK(r.found);
        CHECK_EQ(r.loader, std::string("Fabric"));
        CHECK(!r.suspects.empty());
        if (!r.suspects.empty()) CHECK_EQ(r.suspects[0].modId, std::string("sodium"));
        // fabricloader est de la plateforme : jamais suspect.
        for (const auto& s : r.suspects) CHECK(s.modId != "fabricloader");
    }

    // =====================================================================
    // 4. Mémoire : le mod cité dans la pile n'est pas fautif
    // =====================================================================
    {
        const Report r = analyze_report(kOom);
        CHECK(r.found);
        CHECK(r.cause == Cause::OutOfMemory);
        // Accuser « webscreen » d'un manque de RAM serait une fausse piste
        // qui enverrait l'utilisateur désinstaller un mod innocent.
        CHECK(r.suspects.empty());
        CHECK(r.summary.find("mémoire") != std::string::npos);
    }

    // =====================================================================
    // 4 bis. Cascade : exception de surface sur un mod, vraie cause mémoire
    // =====================================================================
    {
        const Report r = analyze_report(kOomCascade);
        CHECK(r.found);
        // « Caused by: OutOfMemoryError » l'emporte sur le
        // NoClassDefFoundError visible en tête : c'est la racine.
        CHECK(r.cause == Cause::OutOfMemory);
        // Et surtout, OptiFine n'est pas accusé.
        CHECK(r.suspects.empty());
        CHECK(r.summary.find("optifine") == std::string::npos);
    }

    // =====================================================================
    // 5. Listes de mods : les trois formats
    // =====================================================================
    {
        const auto a = parse_mod_list(kForge112);
        CHECK_EQ(a.size(), size_t{3}); // minecraft et forge écartés
        const auto b = parse_mod_list(kForgeModern);
        CHECK_EQ(b.size(), size_t{2});
        const auto c = parse_mod_list(kFabric);
        CHECK_EQ(c.size(), size_t{2}); // fabricloader écarté
        for (const auto& m : c)
            CHECK(m.id == "sodium" || m.id == "iris");
    }

    // =====================================================================
    // 6. Robustesse : rien ne doit lancer
    // =====================================================================
    {
        CHECK(!analyze_report("").found);
        CHECK(!analyze_report("texte sans rapport").found);
        CHECK(!analyze("").has_value());
        // Un rapport tronqué au milieu d'une table.
        CHECK(!analyze_report("| State | ID |\n| LCH |").found);
        // Une ligne d'exception sans aucune liste de mods : cause seule.
        const Report r = analyze_report("java.lang.OutOfMemoryError: heap");
        CHECK(r.found);
        CHECK(r.suspects.empty());
    }

    // =====================================================================
    // 7. Identifiants de cause stables (contrat d'API)
    // =====================================================================
    {
        CHECK_EQ(std::string(cause_key(Cause::OutOfMemory)),
                 std::string("out_of_memory"));
        CHECK_EQ(std::string(cause_key(Cause::ModError)), std::string("mod_error"));
        CHECK_EQ(std::string(cause_key(Cause::Unknown)), std::string("unknown"));
    }

    // =====================================================================
    // 8. Rapports réels (TL_CRASH_DIR=<dossier>)
    // =====================================================================
    if (const char* dir = std::getenv("TL_CRASH_DIR")) {
        std::error_code ec;
        int seen = 0, named = 0;
        for (const auto& e : fs::directory_iterator(fs::path(dir), ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec) || e.path().extension() != ".txt")
                continue;
            std::ifstream in(e.path(), std::ios::binary);
            std::ostringstream ss;
            ss << in.rdbuf();
            const Report r = analyze_report(ss.str());
            ++seen;
            if (!r.suspects.empty()) ++named;
            std::printf("INFO %s\n  cause=%s suspect=%s\n",
                        e.path().filename().string().c_str(), cause_key(r.cause),
                        r.suspects.empty() ? "(aucun)"
                                           : r.suspects[0].modId.c_str());
            // Un vrai rapport doit au minimum être reconnu comme un crash.
            CHECK(r.found);
        }
        std::printf("INFO %d rapport(s) réel(s), %d avec un mod nommé\n", seen,
                    named);
    } else {
        std::printf("INFO rapports réels sautés (TL_CRASH_DIR non défini)\n");
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

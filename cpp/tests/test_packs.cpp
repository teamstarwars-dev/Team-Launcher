// Tests du module 4d : primitives zip, empreinte MurmurHash2 CurseForge,
// assainissement des noms, detection et import de modpacks.
//
// Le test reseau (TL_TEST_NET=1) fait UNE SEULE requete a l'API CurseForge :
// le quota de la cle est limite, on ne le gaspille pas en tests.

#include "curseforge.hpp"
#include "datastore.hpp"
#include "pack_import.hpp"
#include "util_zip.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

#include "miniz.h"

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

// Fabrique une archive zip depuis une liste (nom d'entree, contenu).
static bool make_zip(const fs::path& out,
                     const std::vector<std::pair<std::string, std::string>>& entries) {
    std::error_code ec;
    fs::remove(out, ec);
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, out.string().c_str(), 0)) return false;
    for (const auto& [name, data] : entries)
        mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(),
                              MZ_DEFAULT_COMPRESSION);
    const bool ok = mz_zip_writer_finalize_archive(&z) != 0;
    mz_zip_writer_end(&z);
    return ok;
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-packs-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#endif
    DataStore::load();

    // =====================================================================
    // 1. Primitives zip ajoutees pour les modpacks
    // =====================================================================
    const fs::path z = tmp / "test.zip";
    CHECK(make_zip(z, {{"manifest.json", "{\"name\":\"X\"}"},
                       {"overrides/config/a.cfg", "AAA"},
                       {"overrides/mods/sub/b.txt", "BBB"},
                       {"hors-overrides.txt", "CCC"},
                       {"overrides/../evade.txt", "MECHANT"}}));

    // zip_read_entry
    CHECK_EQ(zip_read_entry(z, "manifest.json").value_or(""),
             std::string("{\"name\":\"X\"}"));
    CHECK(!zip_read_entry(z, "absent.json").has_value());
    CHECK(!zip_read_entry(tmp / "nexistepas.zip", "manifest.json").has_value());

    // zip_extract_prefix : prefixe retire, hierarchie conservee
    const fs::path outDir = tmp / "extrait";
    const int n = zip_extract_prefix(z, "overrides/", outDir);
    CHECK(n >= 2);
    CHECK_EQ(read_file(outDir / "config" / "a.cfg"), std::string("AAA"));
    CHECK_EQ(read_file(outDir / "mods" / "sub" / "b.txt"), std::string("BBB"));
    CHECK(!fs::exists(outDir / "hors-overrides.txt")); // hors prefixe
    // zip-slip : rien n'est ecrit hors du dossier de destination
    CHECK(!fs::exists(tmp / "evade.txt"));
    CHECK(!fs::exists(outDir.parent_path() / "evade.txt"));
    CHECK_EQ(zip_extract_prefix(tmp / "nexistepas.zip", "overrides/", outDir), -1);

    // =====================================================================
    // 2. Empreinte CurseForge (MurmurHash2, variante normalisee)
    // =====================================================================
    // Vecteurs MurmurHash2 avec la graine CurseForge (0x1F123BB5).
    CHECK_EQ(cf::murmur2(""), 0LL);
    CHECK(cf::murmur2("a") != 0LL);
    // Deterministe et sensible au contenu
    CHECK_EQ(cf::murmur2("hello world"), cf::murmur2("hello world"));
    CHECK(cf::murmur2("hello world") != cf::murmur2("hello worle"));
    // Les 4 longueurs residuelles du switch final sont couvertes
    CHECK(cf::murmur2("abcd") != cf::murmur2("abcde"));
    CHECK(cf::murmur2("abcde") != cf::murmur2("abcdef"));
    CHECK(cf::murmur2("abcdef") != cf::murmur2("abcdefg"));
    // Toujours positif (le C# retourne un uint elargi en long)
    for (const char* s : {"a", "ab", "abc", "abcd", "abcdefghij"})
        CHECK(cf::murmur2(s) >= 0);

    // Normalisation : \t \n \r sont retires avant hachage
    {
        const fs::path f1 = tmp / "f1.txt", f2 = tmp / "f2.txt";
        std::ofstream(f1, std::ios::binary) << "ligne1\r\n\tligne2\n";
        std::ofstream(f2, std::ios::binary) << "ligne1ligne2";
        CHECK_EQ(cf::compute_fingerprint(f1), cf::compute_fingerprint(f2));
        CHECK(cf::compute_fingerprint(f1) != 0);
        CHECK_EQ(cf::compute_fingerprint(tmp / "absent.txt"), 0LL);
    }

    // =====================================================================
    // 3. Noms de fichiers et loaders
    // =====================================================================
    CHECK_EQ(cf::sanitize("mod-1.2.3.jar"), std::string("mod-1.2.3.jar"));
    CHECK_EQ(cf::sanitize("a/b\\c:d*e?f\"g<h>i|j"),
             std::string("a_b_c_d_e_f_g_h_i_j"));
    CHECK_EQ(std::string(cf::loader_name(1)), std::string("forge"));
    CHECK_EQ(std::string(cf::loader_name(4)), std::string("fabric"));
    CHECK_EQ(std::string(cf::loader_name(5)), std::string("quilt"));
    CHECK_EQ(std::string(cf::loader_name(6)), std::string("neoforge"));
    CHECK_EQ(std::string(cf::loader_name(99)), std::string(""));

    // =====================================================================
    // 4. Detection de format
    // =====================================================================
    const fs::path mr = tmp / "pack.mrpack";
    CHECK(make_zip(mr, {{"modrinth.index.json", "{\"name\":\"M\"}"}}));
    const fs::path plain = tmp / "plain.zip";
    CHECK(make_zip(plain, {{"lisezmoi.txt", "rien"}}));

    CHECK(packs::detect(z) == packs::Kind::CurseForge);
    CHECK(packs::detect(mr) == packs::Kind::Modrinth);
    CHECK(packs::detect(plain) == packs::Kind::Unknown);
    CHECK(packs::detect(tmp / "nexistepas.zip") == packs::Kind::Unknown);

    std::atomic<bool> cancel{false};
    auto noop = [](const std::string&) {};

    // Format inconnu : erreur claire, pas d'instance creee
    {
        auto r = packs::import_any(plain, noop, cancel);
        CHECK(!r.error.empty());
        CHECK(!r.instance.is_object());
    }

    // =====================================================================
    // 5. Import CurseForge : manifeste sans fichiers (aucun appel API)
    // =====================================================================
    {
        const json manifest = {
            {"name", "Mon Modpack"},
            {"minecraft",
             {{"version", "1.20.1"},
              {"modLoaders", json::array({{{"id", "forge-47.2.0"}}})}}},
            {"files", json::array()}};
        const fs::path cfz = tmp / "cf-vide.zip";
        CHECK(make_zip(cfz, {{"manifest.json", manifest.dump()},
                             {"overrides/config/x.cfg", "CONFIG"},
                             {"overrides/mods/m.jar", "JAR"}}));

        auto r = packs::import_curseforge(cfz, noop, cancel);
        CHECK(r.error.empty());
        CHECK(!r.cancelled);
        CHECK(r.instance.is_object());
        CHECK_EQ(r.instance.value("Name", ""), std::string("Mon Modpack"));
        CHECK_EQ(r.instance.value("Loader", ""), std::string("Forge"));
        CHECK_EQ(r.instance.value("McVersion", ""), std::string("1.20.1"));
        CHECK(r.instance.value("Description", "").find("CurseForge") !=
              std::string::npos);
        CHECK_EQ(r.instance.value("Id", "").size(), size_t{32});
        // overrides/ deposes a la racine de l'instance
        const fs::path dir = DataStore::instancesRoot() / r.instance.value("Id", "");
        CHECK_EQ(read_file(dir / "config" / "x.cfg"), std::string("CONFIG"));
        CHECK_EQ(read_file(dir / "mods" / "m.jar"), std::string("JAR"));
        // L'import n'ecrit PAS dans le config : c'est a l'appelant de le faire
        CHECK(DataStore::settings.instances.empty());
    }

    // Detection du loader : NeoForge l'emporte, Fabric ne coupe pas la boucle
    {
        auto loaderOf = [&](const json& modLoaders) {
            const json m = {{"name", "P"},
                            {"minecraft", {{"version", "1.21"},
                                           {"modLoaders", modLoaders}}},
                            {"files", json::array()}};
            const fs::path p = tmp / "cf-loader.zip";
            make_zip(p, {{"manifest.json", m.dump()}});
            return packs::import_curseforge(p, noop, cancel)
                .instance.value("Loader", "");
        };
        CHECK_EQ(loaderOf(json::array({{{"id", "neoforge-21.0.0"}}})),
                 std::string("NeoForge"));
        CHECK_EQ(loaderOf(json::array({{{"id", "fabric-0.15"}}})),
                 std::string("Fabric"));
        CHECK_EQ(loaderOf(json::array()), std::string("Vanilla"));
        // Fabric puis Forge : le C# prend Forge (break), pas le premier
        CHECK_EQ(loaderOf(json::array({{{"id", "fabric-0.15"}},
                                       {{"id", "forge-47.2.0"}}})),
                 std::string("Forge"));
    }

    // Manifeste absent / illisible
    {
        auto r = packs::import_curseforge(mr, noop, cancel);
        CHECK(!r.error.empty());
        const fs::path bad = tmp / "cf-casse.zip";
        make_zip(bad, {{"manifest.json", "{ pas du json"}});
        auto r2 = packs::import_curseforge(bad, noop, cancel);
        CHECK(!r2.error.empty());
    }

    // =====================================================================
    // 6. Import Modrinth : index sans fichiers (aucun telechargement)
    // =====================================================================
    {
        const json index = {
            {"name", "Pack Modrinth"},
            {"dependencies",
             {{"minecraft", "1.20.1"}, {"fabric-loader", "0.15.11"}}},
            {"files", json::array()}};
        const fs::path p = tmp / "mr-vide.mrpack";
        CHECK(make_zip(p, {{"modrinth.index.json", index.dump()},
                           {"overrides/options.txt", "OPTIONS"}}));

        auto r = packs::import_modrinth(p, noop, cancel);
        CHECK(r.error.empty());
        CHECK_EQ(r.instance.value("Name", ""), std::string("Pack Modrinth"));
        CHECK_EQ(r.instance.value("Loader", ""), std::string("Fabric"));
        // version lue depuis dependencies.minecraft (gameVersion absent)
        CHECK_EQ(r.instance.value("McVersion", ""), std::string("1.20.1"));
        const fs::path dir = DataStore::instancesRoot() / r.instance.value("Id", "");
        CHECK_EQ(read_file(dir / "options.txt"), std::string("OPTIONS"));
    }

    // Chemin de sortie malveillant : compte en echec, rien d'ecrit dehors
    {
        const json index = {
            {"name", "Mechant"},
            {"gameVersion", "1.20.1"},
            {"files", json::array({{{"path", "../../evade.txt"},
                                    {"downloads", json::array({"https://x.invalid/a"})}}})}};
        const fs::path p = tmp / "mr-slip.mrpack";
        make_zip(p, {{"modrinth.index.json", index.dump()}});
        auto r = packs::import_modrinth(p, noop, cancel);
        CHECK(r.error.empty());
        CHECK_EQ(r.failed, 1);
        CHECK_EQ(r.downloaded, 0);
        CHECK(!fs::exists(DataStore::instancesRoot() / "evade.txt"));
        CHECK(!fs::exists(tmp / "evade.txt"));
    }

    // Annulation avant tout travail
    {
        std::atomic<bool> stop{true};
        auto r = packs::import_curseforge(z, noop, stop);
        CHECK(r.cancelled);
    }

    // =====================================================================
    // 7. Cle API : message d'aide si absente, pas d'appel reseau
    // =====================================================================
    {
        const std::string saved = DataStore::settings.curseForgeApiKey;
        DataStore::settings.curseForgeApiKey.clear();
        _putenv_s("CURSEFORGE_API_KEY", "");
        CHECK(!cf::has_key());
        CHECK(std::string(cf::missing_key_message()).find("console.curseforge.com") !=
              std::string::npos);
        bool threw = false;
        try {
            cf::search("test", cf::kClassMods);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw); // pas de requete sans cle
        // Repli sur la variable d'environnement
        _putenv_s("CURSEFORGE_API_KEY", "cle-de-test");
        CHECK(cf::has_key());
        CHECK_EQ(cf::api_key(), std::string("cle-de-test"));
        // Les reglages priment sur l'environnement
        DataStore::settings.curseForgeApiKey = "cle-des-reglages";
        CHECK_EQ(cf::api_key(), std::string("cle-des-reglages"));
        _putenv_s("CURSEFORGE_API_KEY", "");
        DataStore::settings.curseForgeApiKey = saved;
    }

    // =====================================================================
    // 8. Reseau : UNE requete a l'API CurseForge (quota menage)
    // =====================================================================
    if (std::getenv("TL_TEST_NET")) {
        // La cle reelle vient du config utilisateur : on ne la lit que si le
        // test tourne hors TL_DATA_DIR de test. On la prend dans l'env.
        const char* key = std::getenv("TL_CF_KEY");
        if (!key || !*key) {
            std::printf("INFO test CurseForge saute (TL_CF_KEY non defini)\n");
        } else {
            DataStore::settings.curseForgeApiKey = key;
            try {
                const auto hits = cf::search("jei", cf::kClassMods);
                std::printf("INFO recherche CurseForge : %zu resultats\n",
                            hits.size());
                CHECK(!hits.empty());
                if (!hits.empty()) {
                    const auto& h = hits[0];
                    std::printf("INFO 1er : #%d %s (%lld dl) [%s]\n", h.projectId,
                                h.title.c_str(), h.downloads, h.loaders.c_str());
                    CHECK(h.projectId > 0);
                    CHECK(!h.title.empty());
                    CHECK(h.downloads > 0);
                }
            } catch (const std::exception& ex) {
                std::printf("FAIL recherche CurseForge : %s\n", ex.what());
                ++g_failures;
            }
            DataStore::settings.curseForgeApiKey.clear();
        }
    } else {
        std::printf("INFO test reseau saute (TL_TEST_NET non defini)\n");
    }

    DataStore::shutdown();
    fs::remove_all(tmp, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

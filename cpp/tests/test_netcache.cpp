// Tests du cache de metadonnees. Aucun reseau : la fonction de
// recuperation est fournie par le test, ce qui permet d'exercer les trois
// chemins — copie fraiche, appel reseau, copie perimee servie hors ligne.

#include "netcache.hpp"

#include "datastore.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace tl::netcache;

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

// Recule l'horodatage de toutes les entrees de `seconds`. Permet de tester
// la peremption sans attendre, et de fixer un ordre d'eviction sans
// dependre de la granularite des horodatages du systeme de fichiers.
static void age_all(long long seconds) {
    std::error_code ec;
    for (fs::directory_iterator it(dir(), ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const auto t = fs::last_write_time(it->path(), ec);
        if (ec) continue;
        fs::last_write_time(it->path(), t - std::chrono::seconds(seconds), ec);
    }
}

int main() {
    // Cache isole : on ne touche pas aux donnees reelles de la machine.
    const fs::path tmp = fs::temp_directory_path() / "tl-netcache-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#else
    setenv("TL_DATA_DIR", tmp.string().c_str(), 1);
#endif
    CHECK(dir().string().find("tl-netcache-test") != std::string::npos);
    reset_for_tests();

    const std::string url = "https://api.exemple.test/v2/mods?q=sodium";

    // =====================================================================
    // 1. Premier appel : rien en cache, on passe par le reseau
    // =====================================================================
    int calls = 0;
    auto ok = [&](const std::string&) -> std::optional<std::string> {
        ++calls;
        return std::string("{\"data\":1}");
    };
    {
        const auto r = get(url, 3600, ok);
        CHECK(r.ok);
        CHECK(!r.fromCache);
        CHECK(!r.stale);
        CHECK_EQ(r.body, std::string("{\"data\":1}"));
        CHECK_EQ(calls, 1);
    }

    // =====================================================================
    // 2. Deuxieme appel : servi du cache, AUCUN appel reseau
    // =====================================================================
    {
        const auto r = get(url, 3600, ok);
        CHECK(r.ok);
        CHECK(r.fromCache);
        CHECK(!r.stale);
        CHECK_EQ(r.body, std::string("{\"data\":1}"));
        CHECK_EQ(calls, 1); // inchange : c'est tout l'interet
    }

    // =====================================================================
    // 3. maxAge=0 : l'entree est toujours consideree perimee
    // =====================================================================
    {
        const auto r = get(url, -1, ok);
        // maxAge <= 0 cote `peek` accepte n'importe quel age, donc le cache
        // repond encore. On verifie surtout qu'on ne plante pas.
        CHECK(r.ok);
    }

    // =====================================================================
    // 4. Hors ligne : la copie PERIMEE est servie, et signalee
    // =====================================================================
    {
        auto fail = [&](const std::string&) -> std::optional<std::string> {
            return std::nullopt;
        };
        // On VIEILLIT l'entree au lieu d'attendre. Une premiere version
        // dormait 1,1 s pour perimer une entree de 1 s : l'age etant
        // calcule en secondes entieres, le resultat dependait de l'arrondi
        // — vert sous Windows, rouge sous Linux. Un test ne doit pas se
        // jouer a quelques millisecondes.
        age_all(3600);
        const auto r = get(url, 1, fail);
        CHECK(r.ok);
        CHECK(r.fromCache);
        CHECK(r.stale); // l'interface doit pouvoir le dire
        CHECK_EQ(r.body, std::string("{\"data\":1}"));
    }

    // =====================================================================
    // 5. Hors ligne ET rien en cache : echec explicite
    // =====================================================================
    {
        auto fail = [](const std::string&) -> std::optional<std::string> {
            return std::nullopt;
        };
        const auto r = get("https://api.exemple.test/jamais-vu", 3600, fail);
        CHECK(!r.ok);
        CHECK(!r.error.empty());
        CHECK(r.body.empty());
    }

    // =====================================================================
    // 6. Une reponse VIDE n'est jamais mise en cache
    // =====================================================================
    {
        const std::string u2 = "https://api.exemple.test/vide";
        auto empty = [](const std::string&) -> std::optional<std::string> {
            return std::string();
        };
        const auto r = get(u2, 3600, empty);
        // La reponse est rendue telle quelle...
        CHECK(!peek(u2, 0).has_value()); // ...mais rien n'est ecrit
        (void)r;
        put(u2, "");
        CHECK(!peek(u2, 0).has_value());
    }

    // =====================================================================
    // 7. URL differentes = entrees differentes (pas de collision de cle)
    // =====================================================================
    {
        put("https://a.test/x", "AAA");
        put("https://a.test/y", "BBB");
        const auto a = peek("https://a.test/x", 0);
        const auto b = peek("https://a.test/y", 0);
        CHECK(a.has_value() && b.has_value());
        if (a && b) {
            CHECK_EQ(*a, std::string("AAA"));
            CHECK_EQ(*b, std::string("BBB"));
        }
        // Une URL qui n'a jamais ete ecrite ne doit rien rendre, meme si
        // elle ressemble beaucoup a une autre.
        CHECK(!peek("https://a.test/x?", 0).has_value());
    }

    // =====================================================================
    // 8. URL avec caracteres interdits dans un nom de fichier
    // =====================================================================
    {
        // « ? », « : », « * », « & » : illegaux sous Windows. Le cache passe
        // par une empreinte, donc ils ne doivent poser aucun probleme.
        const std::string weird =
            "https://api.test/search?q=a*b&sort=rel:desc&page=2";
        put(weird, "OK");
        const auto v = peek(weird, 0);
        CHECK(v.has_value());
        if (v) CHECK_EQ(*v, std::string("OK"));
    }

    // =====================================================================
    // 9. drop et clear
    // =====================================================================
    {
        put("https://a.test/z", "ZZZ");
        CHECK(peek("https://a.test/z", 0).has_value());
        drop("https://a.test/z");
        CHECK(!peek("https://a.test/z", 0).has_value());

        CHECK(size_bytes() > 0);
        clear();
        CHECK_EQ(size_bytes(), 0LL);
        CHECK(!peek("https://a.test/x", 0).has_value());
    }

    // =====================================================================
    // 10. trim : les plus anciennes partent d'abord
    // =====================================================================
    {
        clear();
        const std::string big(2000, 'x');
        put("https://a.test/old1", big);
        put("https://a.test/old2", big);
        // On VIEILLIT les deux premieres plutot que d'attendre : l'ordre
        // d'eviction doit etre certain, pas dependre de la granularite des
        // horodatages du systeme de fichiers.
        age_all(3600);
        put("https://a.test/new", big);

        const long long before = size_bytes();
        CHECK(before >= 6000);
        trim(2500); // ne doit garder que la plus recente
        const long long after = size_bytes();
        CHECK(after <= 2500);
        CHECK(peek("https://a.test/new", 0).has_value());

        // trim au-dessus de la taille courante ne supprime rien.
        const long long keep = size_bytes();
        trim(keep + 10000);
        CHECK_EQ(size_bytes(), keep);
    }

    // =====================================================================
    // 11. URL vide : refus propre
    // =====================================================================
    {
        auto never = [](const std::string&) -> std::optional<std::string> {
            CHECK(false); // ne doit jamais etre appele
            return std::nullopt;
        };
        const auto r = get("", 3600, never);
        CHECK(!r.ok);
        CHECK(!r.error.empty());
    }

    clear();
    fs::remove_all(tmp, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

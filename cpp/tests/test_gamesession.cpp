// Rattrapage d'une partie dont le launcher n'a pas vu la fin.
//
// Ce module ajoute du temps de jeu sans l'avoir mesuré : il l'estime
// d'après les fichiers que Minecraft a écrits. Le risque n'est donc pas
// qu'il ne compte rien — c'est qu'il compte n'importe quoi. Ces tests
// portent avant tout sur ce qu'il refuse de faire.

#include "datastore.hpp"
#include "gamesession.hpp"

#include "test_env.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#else
#include <unistd.h>
#endif

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>

namespace fs = std::filesystem;
using namespace tl;
using namespace tl::gamesession;

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

static long long now_unix() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

int main() {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "tl_test_session";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    DataStore::load();

    // =====================================================================
    // 1. Bornage de la durée : ce que le module refuse de compter
    // =====================================================================
    CHECK_EQ(clamp_duration(1000, 1600), 600);
    // Fin avant le début : horloge reculée, ou fichier daté du passé.
    CHECK_EQ(clamp_duration(1000, 900), 0);
    CHECK_EQ(clamp_duration(1000, 1000), 0);
    CHECK_EQ(clamp_duration(0, 5000), 0); // pas de début connu
    // Plafond : une « session » de trois jours est un PC resté allumé,
    // pas quelqu'un qui a joué. On préfère sous-compter que raconter
    // n'importe quoi dans les statistiques.
    CHECK_EQ(clamp_duration(1000, 1000 + 72 * 3600), 24 * 3600);
    CHECK_EQ(clamp_duration(1000, 1000 + 24 * 3600), 24 * 3600);

    // =====================================================================
    // 2. Estimation de la fin d'après les fichiers du jeu
    // =====================================================================
    {
        const fs::path inst = tmp / "instances" / "abc";
        fs::create_directories(inst / "logs", ec);
        fs::create_directories(inst / "saves" / "Monde", ec);
        // Dossier sans rien d'exploitable : aucune estimation.
        const fs::path vide = tmp / "instances" / "vide";
        fs::create_directories(vide, ec);
        CHECK_EQ(last_activity_unix(vide), 0);
        CHECK_EQ(last_activity_unix(tmp / "instances" / "nexistepas"), 0);

        std::ofstream(inst / "logs" / "latest.log") << "demarrage";
        const long long t1 = last_activity_unix(inst);
        CHECK(t1 > 0);
        // Un fichier écrit plus tard doit faire avancer l'estimation :
        // c'est tout le principe.
        std::this_thread::sleep_for(std::chrono::milliseconds(1100));
        std::ofstream(inst / "saves" / "Monde" / "level.dat") << "x";
        const long long t2 = last_activity_unix(inst);
        CHECK(t2 >= t1);
        std::printf("INFO estimation : %lld puis %lld\n", t1, t2);
    }

    // =====================================================================
    // 3. Marqueur : écriture, relecture, effacement
    // =====================================================================
    {
        CHECK(!current().has_value());
        CHECK(begin({"abc", 424242, now_unix() - 600}));
        auto s = current();
        CHECK(s.has_value());
        if (s) {
            CHECK_EQ(s->instanceId, std::string("abc"));
            CHECK_EQ(s->pid, 424242UL);
        }
        clear();
        CHECK(!current().has_value());

        // Sans identifiant d'instance, le marqueur ne sert à rien.
        CHECK(!begin({"", 1, now_unix()}));
        // Marqueur corrompu : jeté, pas traîné de démarrage en démarrage.
        { std::ofstream o(marker_path()); o << "{pas du json"; }
        CHECK(!current().has_value());
        CHECK(!fs::exists(marker_path(), ec));
    }

    // =====================================================================
    // 4. Rattrapage complet
    // =====================================================================
    {
        DataStore::settings.instances = nlohmann::json::array({
            {{"Id", "abc"}, {"Name", "Ma partie"}, {"PlaySeconds", 120},
             {"LastPlayed", "0001-01-01T00:00:00"}},
        });
        const fs::path inst = tmp / "instances" / "abc";
        fs::create_directories(inst / "logs", ec);
        std::ofstream(inst / "logs" / "latest.log") << "fin de partie";

        // PID 0 : aucun processus, donc la partie est forcément finie.
        const long long started = now_unix() - 1800; // il y a 30 min
        CHECK(begin({"abc", 0, started}));
        const auto r = reconcile();
        CHECK(r.applied);
        CHECK(!r.stillRunning);
        CHECK_EQ(r.instanceName, std::string("Ma partie"));
        // ~30 min, à quelques secondes près selon l'horodatage du fichier.
        CHECK(r.seconds > 1700 && r.seconds <= 1800);
        CHECK_EQ(DataStore::settings.instances[0]["PlaySeconds"].get<long long>(),
                 120 + r.seconds);
        CHECK(DataStore::settings.instances[0]["LastPlayed"]
                  .get<std::string>()
                  .rfind("0001-", 0) != 0);
        // Le marqueur doit disparaître, sinon le démarrage suivant
        // recompterait la même session.
        CHECK(!fs::exists(marker_path(), ec));
        // Et un second rattrapage ne doit rien ajouter.
        const auto again = reconcile();
        CHECK(!again.applied);
        CHECK_EQ(DataStore::settings.instances[0]["PlaySeconds"].get<long long>(),
                 120 + r.seconds);
    }

    // =====================================================================
    // 5. Une partie qui tourne encore ne doit RIEN déclencher
    // =====================================================================
    {
        // Notre propre processus fait un cobaye parfait : il existe, et
        // sa date de création est connue du système.
#ifdef _WIN32
        const unsigned long self = static_cast<unsigned long>(GetCurrentProcessId());
#else
        const unsigned long self = static_cast<unsigned long>(::getpid());
#endif
        // Date de démarrage volontairement proche de maintenant, pour que
        // le contrôle « même processus » reconnaisse le nôtre.
        CHECK(begin({"abc", self, now_unix()}));
        const auto r = reconcile();
        CHECK(r.stillRunning);
        CHECK(!r.applied);
        // Le marqueur reste : on réessaiera au prochain démarrage.
        CHECK(fs::exists(marker_path(), ec));
        clear();
    }

    // =====================================================================
    // 6. Instance disparue entre-temps
    // =====================================================================
    {
        CHECK(begin({"instance-supprimee", 0, now_unix() - 600}));
        const auto r = reconcile();
        CHECK(!r.applied); // rien à créditer, et surtout pas de plantage
        CHECK(!fs::exists(marker_path(), ec));
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

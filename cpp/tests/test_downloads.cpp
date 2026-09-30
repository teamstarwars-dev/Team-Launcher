// Tests de la file de telechargements. Aucun acces reseau : la fonction de
// recuperation est remplacee par une fausse, ce qui permet d'exercer la
// mecanique de file elle-meme — limite de parallelisme, annulation, relance,
// compteurs, progression.

#include "downloads.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

using namespace tl::downloads;

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

// Attend une condition, avec un plafond : un test ne doit jamais rester
// bloque, meme si la mecanique est cassee.
template <typename F>
static bool wait_until(F f, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        if (f()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return f();
}

int main() {
    // =====================================================================
    // 1. Refus des entrees vides
    // =====================================================================
    reset_for_tests();
    CHECK_EQ(enqueue("x", "", "/tmp/a"), 0);
    CHECK_EQ(enqueue("x", "https://exemple/a", ""), 0);
    CHECK_EQ(snapshot().size(), std::size_t{0});

    // =====================================================================
    // 2. Deroulement nominal : progression puis Done
    // =====================================================================
    {
        reset_for_tests();
        set_fetcher([](const std::string&, const std::filesystem::path&,
                       const ProgressFn& p, const std::atomic<bool>&,
                       std::string*) {
            if (p) {
                p(50, 100);
                p(100, 100);
            }
            return true;
        });
        const int id = enqueue("Sodium", "https://exemple.test/sodium.jar",
                               "/tmp/sodium.jar");
        CHECK(id > 0);
        CHECK(wait_until([] { return count_finished() == 1; }));
        const auto s = snapshot();
        CHECK_EQ(s.size(), std::size_t{1});
        if (!s.empty()) {
            CHECK(s[0].state == State::Done);
            CHECK_EQ(s[0].done, 100LL);
            CHECK_EQ(s[0].total, 100LL);
            CHECK(s[0].progress() > 0.99);
            CHECK_EQ(s[0].label, std::string("Sodium"));
            CHECK(s[0].error.empty());
            CHECK(s[0].endedUnix > 0);
        }
        CHECK_EQ(count_active(), std::size_t{0});
    }

    // =====================================================================
    // 3. Libelle vide : le nom du fichier sert de libelle
    // =====================================================================
    {
        reset_for_tests();
        set_fetcher([](const std::string&, const std::filesystem::path&,
                       const ProgressFn&, const std::atomic<bool>&,
                       std::string*) { return true; });
        enqueue("", "https://exemple.test/x", "/tmp/monfichier.zip");
        CHECK(wait_until([] { return count_finished() == 1; }));
        const auto s = snapshot();
        if (!s.empty()) CHECK_EQ(s[0].label, std::string("monfichier.zip"));
    }

    // =====================================================================
    // 4. Echec : etat Failed et message conserve
    // =====================================================================
    {
        reset_for_tests();
        set_fetcher([](const std::string&, const std::filesystem::path&,
                       const ProgressFn&, const std::atomic<bool>&,
                       std::string* err) {
            if (err) *err = "404";
            return false;
        });
        const int id = enqueue("absent", "https://exemple.test/absent", "/tmp/a");
        CHECK(wait_until([] { return count_finished() == 1; }));
        auto s = snapshot();
        CHECK(!s.empty() && s[0].state == State::Failed);
        if (!s.empty()) CHECK_EQ(s[0].error, std::string("404"));

        // --- 4bis. Relance d'un echec ---
        set_fetcher([](const std::string&, const std::filesystem::path&,
                       const ProgressFn&, const std::atomic<bool>&,
                       std::string*) { return true; });
        CHECK(retry(id));
        CHECK(wait_until([] {
            auto v = snapshot();
            return !v.empty() && v[0].state == State::Done;
        }));
        s = snapshot();
        if (!s.empty()) CHECK(s[0].error.empty());
        // Un element termine n'est pas relancable, un id inconnu non plus.
        CHECK(!retry(id));
        CHECK(!retry(9999));
    }

    // =====================================================================
    // 5. Limite de parallelisme reellement respectee
    // =====================================================================
    {
        reset_for_tests();
        std::atomic<int> concurrent{0};
        std::atomic<int> peak{0};
        std::atomic<bool> release{false};
        set_fetcher([&](const std::string&, const std::filesystem::path&,
                        const ProgressFn&, const std::atomic<bool>&,
                        std::string*) {
            const int c = ++concurrent;
            int prev = peak.load();
            while (c > prev && !peak.compare_exchange_weak(prev, c)) {
            }
            while (!release.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            --concurrent;
            return true;
        });

        set_limit(3);
        CHECK_EQ(limit(), 3);
        for (int i = 0; i < 12; ++i)
            enqueue("f" + std::to_string(i), "https://exemple.test/f", "/tmp/f");

        // Laisse le temps a d'eventuels travailleurs excedentaires de
        // demarrer : c'est justement ce qu'on veut interdire.
        CHECK(wait_until([&] { return concurrent.load() == 3; }));
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        CHECK_EQ(peak.load(), 3);

        release = true;
        CHECK(wait_until([] { return count_finished() == 12; }, 10000));
        CHECK_EQ(peak.load(), 3);
        CHECK_EQ(count_active(), std::size_t{0});
    }

    // =====================================================================
    // 6. Bornes de la limite
    // =====================================================================
    {
        set_limit(0);
        CHECK_EQ(limit(), 1); // jamais zero, sinon plus rien ne part
        set_limit(999);
        CHECK_EQ(limit(), 20);
        set_limit(4);
    }

    // =====================================================================
    // 7. Annulation : en attente et en cours
    // =====================================================================
    {
        reset_for_tests();
        std::atomic<bool> release{false};
        set_fetcher([&](const std::string&, const std::filesystem::path&,
                        const ProgressFn&, const std::atomic<bool>& cancel,
                        std::string*) {
            while (!release.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return !cancel.load();
        });
        set_limit(1);
        const int first = enqueue("en-cours", "https://exemple.test/a", "/tmp/a");
        const int second = enqueue("en-attente", "https://exemple.test/b", "/tmp/b");

        // Le second doit rester en file tant que le premier occupe la place.
        CHECK(wait_until([&] {
            auto v = snapshot();
            for (const auto& x : v)
                if (x.id == first && x.state == State::Running) return true;
            return false;
        }));

        // Annuler un element EN ATTENTE : il ne doit jamais demarrer.
        CHECK(cancel(second));
        // Annuler un element EN COURS : le drapeau parvient au transport.
        CHECK(cancel(first));
        CHECK(wait_until([] { return count_active() == 0; }));

        for (const auto& x : snapshot()) {
            CHECK(x.state == State::Cancelled);
            CHECK(!x.error.empty());
        }
        CHECK(!cancel(first));   // deja termine
        CHECK(!cancel(123456));  // inconnu

        // Un element annule se relance.
        release = true;
        CHECK(retry(first));
        CHECK(wait_until([&] {
            for (const auto& x : snapshot())
                if (x.id == first) return x.state == State::Done;
            return false;
        }));
    }

    // =====================================================================
    // 8. Compteurs, tri et nettoyage
    // =====================================================================
    {
        reset_for_tests();
        set_fetcher([](const std::string&, const std::filesystem::path&,
                       const ProgressFn&, const std::atomic<bool>&,
                       std::string*) { return true; });
        for (int i = 0; i < 5; ++i)
            enqueue("f" + std::to_string(i), "https://exemple.test/f", "/tmp/f");
        CHECK(wait_until([] { return count_finished() == 5; }));

        const auto s = snapshot();
        CHECK_EQ(s.size(), std::size_t{5});
        // Du plus recent au plus ancien.
        for (std::size_t i = 1; i < s.size(); ++i) CHECK(s[i - 1].id > s[i].id);

        const auto before = version();
        clear_finished();
        CHECK_EQ(snapshot().size(), std::size_t{0});
        CHECK_EQ(count_finished(), std::size_t{0});
        CHECK(version() > before); // l'interface sait qu'il faut redessiner
    }

    // =====================================================================
    // 9. L'URL exposee a l'interface est REDIGEE
    // =====================================================================
    {
        reset_for_tests();
        set_fetcher([](const std::string& url, const std::filesystem::path&,
                       const ProgressFn&, const std::atomic<bool>&,
                       std::string*) {
            // Le transport, lui, recoit bien l'URL complete.
            return url.find("SECRET") != std::string::npos;
        });
        enqueue("jeton", "https://exemple.test/f?token=SECRET", "/tmp/f");
        CHECK(wait_until([] { return count_finished() == 1; }));
        const auto s = snapshot();
        CHECK(!s.empty() && s[0].state == State::Done); // le transport a bien recu le jeton
        if (!s.empty()) {
            CHECK(s[0].url.find("SECRET") == std::string::npos);
            CHECK(s[0].url.find("token=***") != std::string::npos);
        }
    }

    // =====================================================================
    // 10. wait_idle
    // =====================================================================
    {
        reset_for_tests();
        set_fetcher([](const std::string&, const std::filesystem::path&,
                       const ProgressFn&, const std::atomic<bool>&,
                       std::string*) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            return true;
        });
        CHECK(wait_idle(0)); // deja vide
        for (int i = 0; i < 6; ++i)
            enqueue("f", "https://exemple.test/f", "/tmp/f");
        CHECK(wait_idle(10000));
        CHECK_EQ(count_active(), std::size_t{0});
    }

    // =====================================================================
    // 11. Persistance : la file survit a un redemarrage
    // =====================================================================
    {
        // Etat isole : on ne touche pas au fichier reel de la machine.
        const auto tmp = std::filesystem::temp_directory_path() / "tl-dl-state";
        std::error_code ec;
        std::filesystem::remove_all(tmp, ec);
        std::filesystem::create_directories(tmp, ec);
#ifdef _WIN32
        _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#else
        setenv("TL_DATA_DIR", tmp.string().c_str(), 1);
#endif
        reset_for_tests();
        std::filesystem::remove(state_path(), ec);

        // Un transport qui bloque : les elements restent en cours/en file,
        // c'est l'etat qu'on veut retrouver apres un « plantage ».
        std::atomic<bool> hold{true};
        set_fetcher([&](const std::string&, const std::filesystem::path&,
                        const ProgressFn&, const std::atomic<bool>& cancel,
                        std::string*) {
            while (hold.load() && !cancel.load())
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            return !cancel.load();
        });
        set_limit(1);
        enqueue("mod A", "https://exemple.test/a.jar", "/tmp/a.jar");
        enqueue("mod B", "https://exemple.test/b.jar", "/tmp/b.jar");
        // URL PORTEUSE DE SECRET : ne doit jamais atterrir sur le disque.
        enqueue("prive", "https://exemple.test/c.jar?token=SECRET", "/tmp/c.jar");
        CHECK(wait_until([] { return count_active() == 3; }));

        CHECK(std::filesystem::exists(state_path()));
        {
            std::ifstream in(state_path());
            const std::string txt((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            CHECK(txt.find("mod A") != std::string::npos);
            CHECK(txt.find("mod B") != std::string::npos);
            // Ni le jeton, ni meme l'entree.
            CHECK(txt.find("SECRET") == std::string::npos);
            CHECK(txt.find("prive") == std::string::npos);
        }

        // « Plantage » : on coupe sans vider le fichier d'etat. On garde
        // une COPIE du fichier, car reset_for_tests repart d'un moteur
        // vide et la prochaine ecriture l'ecraserait.
        const auto saved = tmp / "downloads.copie.json";
        std::filesystem::copy_file(
            state_path(), saved,
            std::filesystem::copy_options::overwrite_existing, ec);
        hold = false;
        reset_for_tests();
        CHECK_EQ(snapshot().size(), std::size_t{0});
        std::filesystem::copy_file(
            saved, state_path(),
            std::filesystem::copy_options::overwrite_existing, ec);

        {
            // Transport bloquant : les elements repris restent en cours, on
            // peut donc observer l'etat sans course.
            std::atomic<bool> hold2{true};
            set_fetcher([&](const std::string&, const std::filesystem::path&,
                            const ProgressFn&, const std::atomic<bool>& cancel,
                            std::string*) {
                while (hold2.load() && !cancel.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                return false;
            });
            load_state();
            const auto s = snapshot();
            // Les deux elements sans secret reviennent ; celui qui portait
            // un jeton n'a jamais ete ecrit, donc il ne revient pas.
            CHECK_EQ(s.size(), std::size_t{2});
            for (const auto& x : s) CHECK(x.label != "prive");
            // Ils sont A REFAIRE : en attente ou relances, jamais « termine ».
            CHECK_EQ(count_active(), std::size_t{2});
            hold2 = false;
            cancel_all();
            wait_idle(5000);
        }

        // Un fichier d'etat illisible ne doit pas empecher le demarrage.
        reset_for_tests();
        {
            std::ofstream out(state_path(), std::ios::trunc);
            out << "{ ceci n'est pas du json";
        }
        load_state(); // ne doit ni planter ni lever
        CHECK_EQ(snapshot().size(), std::size_t{0});

        std::filesystem::remove_all(tmp, ec);
    }

    shutdown();

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

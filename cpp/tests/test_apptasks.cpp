// Tests hors ligne du portage AppTasks.cs : cycle de vie complet,
// annulation, snapshot, concurrence (creations simultanees, comptes exacts).

#include "apptasks.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace tl::tasks;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                     \
    do {                                                                   \
        const auto va = (a);                                               \
        const auto vb = (b);                                               \
        if (!(va == vb)) {                                                 \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a,   \
                        #b);                                               \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

static const Info* find_by_id(const std::vector<Info>& v, int id) {
    for (const auto& i : v)
        if (i.id == id) return &i;
    return nullptr;
}

int main() {
    // --- 1. Cycle de vie complet : create -> update -> finish ---
    reset_for_tests();
    CHECK_EQ(count(), std::size_t(0));
    const std::uint64_t v0 = version();
    const int id = create("Import pack", "Demarrage...");
    CHECK(id > 0);
    CHECK_EQ(count(), std::size_t(1));
    CHECK_EQ(count_running(), std::size_t(1));
    CHECK(version() > v0);
    {
        auto s = snapshot();
        CHECK_EQ(s.size(), std::size_t(1));
        const Info* f = find_by_id(s, id);
        CHECK(f != nullptr);
        if (f) {
            CHECK_EQ(f->title, std::string("Import pack"));
            CHECK_EQ(f->status, std::string("Demarrage..."));
            CHECK(f->state == State::Running);
            CHECK_EQ(f->progress, 0.0);
            CHECK(!f->cancelRequested);
            CHECK(f->startedUnix > 0);
        }
    }
    update(id, "A mi-chemin", 0.5);
    {
        auto s = snapshot();
        const Info* f = find_by_id(s, id);
        CHECK(f != nullptr);
        if (f) {
            CHECK_EQ(f->status, std::string("A mi-chemin"));
            CHECK_EQ(f->progress, 0.5);
        }
    }
    // Progression hors bornes : clamp 0..1.
    update(id, "Presque", 1.5);
    CHECK_EQ(find_by_id(snapshot(), id)->progress, 1.0);
    update(id, "Recule", -0.5); // negatif = ignore (-1 = inchange par API)
    CHECK_EQ(find_by_id(snapshot(), id)->progress, 1.0);
    CHECK(finish(id));
    CHECK_EQ(count_running(), std::size_t(0));
    {
        auto snapDone = snapshot(); // le snapshot doit survivre au pointeur
        const Info* f = find_by_id(snapDone, id);
        CHECK(f != nullptr && f->state == State::Done);
    }
    // update/finish sur terminee : sans effet, pas de crash.
    update(id, "Trop tard", 0.0);
    CHECK_EQ(find_by_id(snapshot(), id)->state, State::Done);
    // Id inconnu : sans effet, pas de crash.
    update(999999, "x", 0.5);
    CHECK(!finish(999999));
    CHECK(!fail(999999, "x"));
    CHECK(!cancel(999999));
    CHECK(!cancelled(999999));

    // --- 2. Echec : fail(message) ---
    reset_for_tests();
    const int kf = create("Installation", "Telechargement...");
    CHECK(fail(kf, "Reseau indisponible"));
    {
        auto snapFail = snapshot();
        const Info* f = find_by_id(snapFail, kf);
        CHECK(f != nullptr && f->state == State::Failed);
        if (f) CHECK_EQ(f->status, std::string("Reseau indisponible"));
    }
    // fail sur terminee : sans effet.
    CHECK(fail(kf, "autre"));
    CHECK_EQ(find_by_id(snapshot(), kf)->status,
             std::string("Reseau indisponible"));

    // --- 3. Annulation : drapeau + etat ---
    reset_for_tests();
    const int kc = create("Exploration", "Scan...");
    CHECK(!cancelled(kc));
    CHECK(cancel(kc));
    CHECK(cancelled(kc));
    {
        auto snapCancel = snapshot();
        const Info* f = find_by_id(snapCancel, kc);
        CHECK(f != nullptr && f->state == State::Cancelled);
        if (f) CHECK(f->cancelRequested);
    }

    // --- 4. Snapshot : copie isolee, triee par id ---
    reset_for_tests();
    const int a = create("B", "s");
    const int b = create("A", "s");
    CHECK(b > a);
    auto snap = snapshot();
    CHECK_EQ(snap.size(), std::size_t(2));
    CHECK(snap[0].id < snap[1].id);
    snap[0].title = "MUTEE"; // ne doit pas fuir dans le registre
    CHECK_EQ(find_by_id(snapshot(), snap[1].id)->title,
             std::string(snap[1].id == a ? "B" : "A"));

    // --- 5. clear_finished : ne garde que les Running ---
    reset_for_tests();
    const int r1 = create("Vivante", "s");
    const int r2 = create("Finie", "s");
    const int r3 = create("Ratee", "s");
    const int r4 = create("Stoppee", "s");
    CHECK(finish(r2));
    CHECK(fail(r3, "boom"));
    CHECK(cancel(r4));
    CHECK_EQ(count(), std::size_t(4));
    clear_finished();
    CHECK_EQ(count(), std::size_t(1));
    CHECK_EQ(find_by_id(snapshot(), r1)->state, State::Running);

    // --- 6. run() : succes, echec, on_error, annulation ---
    reset_for_tests();
    {
        auto fut = run("Job ok",
                       [](const std::atomic<bool>&,
                          const std::function<void(const std::string&, double)>& set) {
                           set("Moitié", 0.5);
                           set("Fin", 1.0);
                       });
        fut.get();
        auto s = snapshot();
        CHECK_EQ(s.size(), std::size_t(1));
        CHECK(s[0].state == State::Done);
        CHECK_EQ(s[0].status, std::string("Fin"));
        CHECK_EQ(s[0].progress, 1.0);
    }
    {
        bool errCalled = false;
        auto fut = run("Job ko",
                       [](const std::atomic<bool>&,
                          const std::function<void(const std::string&, double)>&) {
                           throw std::runtime_error("panne simulee");
                       },
                       [&](std::exception_ptr) { errCalled = true; });
        fut.get();
        CHECK(errCalled);
        auto s = snapshot();
        CHECK_EQ(s.size(), std::size_t(2));
        const Info* f = find_by_id(s, s.back().id);
        CHECK(f != nullptr && f->state == State::Failed);
        if (f) CHECK_EQ(f->status, std::string("panne simulee"));
    }
    {
        auto fut = run("Job annule",
                       [](const std::atomic<bool>& cancel,
                          const std::function<void(const std::string&, double)>& set) {
                           set("Debut", 0.1);
                           // Simule un worker qui voit l'annulation et s'arrete.
                           (void)cancel.load();
                       });
        fut.get();
        // Etat terminal conserve pour affichage (ecart assume vs C# qui
        // retirait l'entree) : l'entree reste visible en Done.
        auto s = snapshot();
        CHECK_EQ(s.size(), std::size_t(3));
        CHECK(find_by_id(s, s.back().id)->state == State::Done);
    }

    // --- 7. Concurrence : creations simultanees, aucun crash, comptes exacts ---
    reset_for_tests();
    {
        constexpr int kThreads = 8;
        constexpr int kPerThread = 50;
        std::vector<std::thread> th;
        std::atomic<int> updated{0};
        for (int t = 0; t < kThreads; ++t) {
            th.emplace_back([t, &updated] {
                for (int i = 0; i < kPerThread; ++i) {
                    const int id2 =
                        create("T" + std::to_string(t) + "-" + std::to_string(i),
                               "s");
                    update(id2, "p", 0.25);
                    ++updated;
                    (void)snapshot();
                    (void)count_running();
                    (void)version();
                }
            });
        }
        for (auto& t : th) t.join();
        CHECK_EQ(updated.load(), kThreads * kPerThread);
        CHECK_EQ(count(), std::size_t(kThreads * kPerThread));
        CHECK_EQ(count_running(), std::size_t(kThreads * kPerThread));
        // Ids uniques.
        auto s = snapshot();
        std::vector<int> ids;
        for (const auto& i : s) ids.push_back(i.id);
        std::sort(ids.begin(), ids.end());
        bool unique = true;
        for (std::size_t i = 1; i < ids.size(); ++i)
            if (ids[i] == ids[i - 1]) unique = false;
        CHECK(unique);
        // Terminaison concurrente : moitie finish, moitie fail.
        std::vector<std::thread> th2;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            th2.emplace_back([idv = ids[i], i] {
                if (i % 2 == 0)
                    (void)finish(idv);
                else
                    (void)fail(idv, "e");
            });
        }
        for (auto& t : th2) t.join();
        CHECK_EQ(count_running(), std::size_t(0));
        clear_finished();
        CHECK_EQ(count(), std::size_t(0));
    }

    reset_for_tests();
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

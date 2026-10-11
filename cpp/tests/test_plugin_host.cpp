// Système de plugins V8, de bout en bout : vrai launcher (client), vrai
// tl_plugin_host, faux plugin hostile ou non.
//
// Ce test lance de vrais processus. Il vérifie les garanties promises dans
// plugin_proto.hpp et plugin_host_client.hpp, pas leur intention :
//   - un appel permis atteint le gestionnaire du launcher ;
//   - un appel NON permis ne l'atteint JAMAIS ;
//   - aucun appel n'est accepté avant host.hello ;
//   - une méthode hors table est refusée ;
//   - une trame mensongère, ou une permission sondée à répétition, fait
//     TUER le plugin ;
//   - une rafale déclenche la limite de débit ;
//   - le kill-switch local arrête tout.

#include "plugin_host_client.hpp"
#include "plugin_proto.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
namespace ph = tl::pluginhost;
using nlohmann::json;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

fs::path g_dir; // dossier des exécutables construits

fs::path exe(const char* name) {
#ifdef _WIN32
    return g_dir / (std::string(name) + ".exe");
#else
    return g_dir / name;
#endif
}

struct Observed {
    std::vector<std::string> logs;
    std::vector<ph::Event> events;
    std::vector<std::string> handled; // méthodes parvenues au launcher
};

// Le gestionnaire du « launcher » : deux instances factices.
json handler(const ph::Call& c, int* code, std::string* msg, Observed& o) {
    o.handled.push_back(c.method);
    if (c.method == "instances.list")
        return json{{"instances", json::array({json{{"id", std::string(32, 'a')}},
                                               json{{"id", std::string(32, 'b')}}})}};
    *code = tl::pluginproto::kInternal;
    *msg = "non géré par le test";
    return json();
}

bool has_log(const Observed& o, const std::string& s) {
    for (const auto& l : o.logs)
        if (l.find(s) != std::string::npos) return true;
    return false;
}

bool has_event(const Observed& o, const std::string& plugin, const std::string& kind) {
    for (const auto& e : o.events)
        if (e.plugin == plugin && e.kind == kind) return true;
    return false;
}

bool handled(const Observed& o, const std::string& m) {
    for (const auto& h : o.handled)
        if (h == m) return true;
    return false;
}

// Lance un faux plugin et pompe jusqu'à `until` (ou expiration).
Observed run(const std::string& id, const std::string& mode,
             const std::vector<std::string>& perms,
             bool (*until)(const Observed&, const std::string&), int timeoutMs = 8000) {
    Observed o;
    std::string err;
    ph::Launch l;
    l.id = id;
    l.program = exe("tl_test_plugin").string();
    l.args = {mode};
    l.permissions = perms;
    if (!ph::start_plugin(l, &err)) {
        std::printf("start_plugin a échoué : %s\n", err.c_str());
        ++g_failures;
        return o;
    }
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < end) {
        ph::pump(
            [&](const ph::Call& c, int* code, std::string* msg) {
                return handler(c, code, msg, o);
            },
            [&](const ph::Event& e) {
                o.events.push_back(e);
                if (e.kind == "log") o.logs.push_back(e.detail);
            });
        if (until(o, id)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ph::stop_plugin(id);
    std::printf("INFO %-8s %5lld ms\n", id.c_str(),
                static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - (end - std::chrono::milliseconds(timeoutMs))).count()));
    return o;
}

bool ended(const Observed& o, const std::string& id) {
    return has_event(o, id, "exited") || has_event(o, id, "killed") ||
           has_event(o, id, "failed");
}

} // namespace

int main(int, char** argv) {
    g_dir = fs::absolute(fs::path(argv[0])).parent_path();
    std::string err;
    if (!ph::start(exe("tl_plugin_host"), &err)) {
        std::printf("FAIL : hôte non démarré : %s\n", err.c_str());
        return 1;
    }
    CHECK(ph::running());

    // 1. Parcours nominal : permis accepté, non permis refusé ET absent du
    //    launcher.
    {
        const auto o = run("nominal", "normal", {"instances:read"}, ended);
        CHECK(has_log(o, "LIST n=2"));
        CHECK(has_log(o, "DENIED code=1001"));
        CHECK(has_log(o, "FIN"));
        CHECK(handled(o, "instances.list"));
        // LA garantie : la méthode refusée n'a jamais atteint le launcher.
        CHECK(!handled(o, "game.launch"));
        CHECK(has_event(o, "nominal", "denied"));
        CHECK(has_event(o, "nominal", "exited"));
    }

    // 2. Rien avant host.hello.
    {
        const auto o = run("nohello", "nohello", {"instances:read"}, ended);
        CHECK(has_log(o, "NOHELLO code=-32600"));
        CHECK(!handled(o, "instances.list"));
    }

    // 3. Méthode hors table.
    {
        const auto o = run("forge", "forge", {"instances:read", "game:launch"}, ended);
        CHECK(has_log(o, "FORGE code=-32601"));
        CHECK(o.handled.empty());
    }

    // 4. Trame mensongère : le plugin est tué, pas toléré.
    {
        const auto o = run("garbage", "garbage", {}, ended);
        CHECK(has_event(o, "garbage", "killed"));
        CHECK(o.handled.empty());
    }

    // 5. Permission sondée à répétition : tué.
    {
        const auto o = run("probe", "probe", {"instances:read"}, ended);
        CHECK(has_event(o, "probe", "killed"));
        CHECK(!has_log(o, "PROBE survived"));
        CHECK(!handled(o, "game.launch"));
    }

    // 6. Rafale : la limite de débit se déclenche.
    {
        const auto o = run("flood", "flood", {"instances:read"}, ended, 15000);
        bool limited = false;
        for (const auto& l : o.logs)
            if (l.find("FLOOD limited=") != std::string::npos &&
                l.find("FLOOD limited=0") == std::string::npos)
                limited = true;
        CHECK(limited);
    }

    // 7. Le launcher revalide : un plugin qu'il n'a pas démarré n'existe
    //    pas pour lui, même si le host le prétendait. (Le cas est couvert
    //    par revalidate ; ici on vérifie qu'un plugin arrêté ne voit plus
    //    ses appels honorés.)
    {
        ph::Launch l;
        l.id = "arrete";
        l.program = exe("tl_test_plugin").string();
        l.args = {"normal"};
        l.permissions = {"instances:read"};
        ph::start_plugin(l, &err);
        ph::stop_plugin("arrete");
        Observed o;
        for (int i = 0; i < 50; ++i) {
            ph::pump([&](const ph::Call& c, int* code, std::string* msg) {
                         return handler(c, code, msg, o);
                     },
                     nullptr);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(!handled(o, "instances.list"));
    }

    // 8. Kill-switch local : tout s'arrête.
    {
        ph::Launch l;
        l.id = "victime";
        l.program = exe("tl_test_plugin").string();
        l.args = {"idle"}; // reste en vie 20 s s'il n'est pas tué
        ph::start_plugin(l, &err);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        ph::kill_all();
        Observed o;
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < end && !has_event(o, "", "killall")) {
            ph::pump(nullptr, [&](const ph::Event& e) { o.events.push_back(e); });
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(has_event(o, "", "killall"));
    }

    ph::stop();
    CHECK(!ph::running());

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

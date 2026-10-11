// Télémétrie anonyme des plugins (V8) : raisons fermées, plafonds, et
// document conforme à ce qu'accepte le serveur.

#include "plugin_telemetry.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

using namespace tl;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main() {
    // Raisons : liste fermée, aucun texte libre ne peut passer.
    CHECK(plugtele::classify("killed", "débit abusif") == "rate_abuse");
    CHECK(plugtele::classify("killed", "permissions sondées à répétition") == "permission_probe");
    CHECK(plugtele::classify("killed", "requêtes invalides répétées") == "invalid_requests");
    CHECK(plugtele::classify("killed", "trame invalide") == "bad_frame");
    CHECK(plugtele::classify("killed", "C:\\Users\\Jean\\secret.txt") == "killed_other");
    CHECK(plugtele::classify("denied", "game.launch") == "denied");
    CHECK(plugtele::classify("failed", "introuvable") == "start_failed");
    CHECK(plugtele::classify("exited", "") == "exited");
    CHECK(plugtele::classify("started", "").empty());
    CHECK(plugtele::classify("host_died", "").empty());

    // Agrégation.
    plugtele::reset_for_tests();
    for (int i = 0; i < 3; ++i) plugtele::record("hello", "1.0.0", "denied");
    plugtele::record("hello", "1.0.0", "rate_abuse");
    plugtele::record("hello", "", "denied");  // incomplet : ignoré
    plugtele::record("hello", "1.0.0", "");   // événement non compté
    CHECK(plugtele::pending() == 2);

    // Document : exactement les champs attendus par le serveur.
    plugtele::Counts c = plugtele::take();
    CHECK(plugtele::pending() == 0);
    const auto doc = nlohmann::json::parse(plugtele::payload(c, "8.0.0", "windows"));
    CHECK(doc["v"] == 1 && doc["launcher"] == "8.0.0" && doc["os"] == "windows");
    CHECK(doc["events"].size() == 2);
    for (const auto& e : doc["events"]) {
        CHECK(e.size() == 4); // plugin, version, kind, count — rien d'autre
        CHECK(e["plugin"] == "hello" && e["version"] == "1.0.0");
        CHECK(e["kind"] == "denied" ? e["count"] == 3 : e["count"] == 1);
    }

    // Échec d'envoi : remis en attente et fusionné avec le nouveau.
    plugtele::record("hello", "1.0.0", "denied");
    plugtele::restore(c);
    c = plugtele::take();
    CHECK((c[{"hello", "1.0.0", "denied"}] == 4));

    // Plafonds : 200 clés au plus, 10 000 par clé.
    plugtele::reset_for_tests();
    for (int i = 0; i < 300; ++i) plugtele::record("p" + std::to_string(i), "1", "denied");
    CHECK(plugtele::pending() == 200);
    plugtele::reset_for_tests();
    plugtele::Counts big;
    big[{"a", "1", "denied"}] = 9999;
    plugtele::restore(big);
    plugtele::restore(big);
    CHECK((plugtele::take()[{"a", "1", "denied"}] == 10000));

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

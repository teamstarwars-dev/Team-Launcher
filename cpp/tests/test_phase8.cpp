// Phase 8 : plugins (manifeste, substitution, garde-fous) et API HTTP
// locale (jeton, points d'entrée, file de lancement).
//
// Le préchauffage n'est pas testé ici : il n'a aucun effet observable par
// construction — il remplit un cache et chauffe celui du système. Le seul
// comportement qui compte, « find_java ne rebalaye pas deux fois », est
// vérifié en mesurant le second appel.

#include "apievents.hpp"
#include "apikeys.hpp"
#include "datastore.hpp"
#include "game_launcher.hpp"
#include "localapi.hpp"
#include "plugins.hpp"
#include "util_hash.hpp"

#include "test_env.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <httplib.h>

namespace fs = std::filesystem;
using namespace tl;

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

static void write_plugin(const fs::path& root, const std::string& id,
                         const std::string& manifest) {
    std::error_code ec;
    fs::create_directories(root / id, ec);
    std::ofstream(root / id / "plugin.json", std::ios::binary) << manifest;
}

int main() {
    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "tl_test_phase8";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    DataStore::load();

    // =====================================================================
    // 1. Manifeste : ce qui est accepté
    // =====================================================================
    {
        const auto p = plugins::parse_manifest(R"({
            "name": "Outils", "version": "1.2", "author": "Moi",
            "description": "Deux actions",
            "actions": [
              { "label": "Dossier mods", "where": "instance",
                "run": "explorer", "args": ["{instanceDir}/mods"] },
              { "label": "Bloc-notes", "run": "notepad" }
            ]
        })",
                                               "outils");
        CHECK(p.error.empty());
        CHECK_EQ(p.id, std::string("outils"));
        CHECK_EQ(p.name, std::string("Outils"));
        CHECK_EQ(p.version, std::string("1.2"));
        CHECK_EQ(p.actions.size(), size_t{2});
        if (p.actions.size() == 2) {
            CHECK(p.actions[0].where == plugins::Where::Instance);
            // « where » absent = outils, pas instance : une action qui
            // réclamerait une instance sans le dire serait grisée à tort.
            CHECK(p.actions[1].where == plugins::Where::Tools);
        }
        CHECK(!p.manifestHash.empty());
        // Un plugin n'est JAMAIS autorisé du seul fait d'exister.
        CHECK(!p.enabled);
    }

    // =====================================================================
    // 2. Manifeste : ce qui est refusé, et dit
    // =====================================================================
    {
        const auto bad = plugins::parse_manifest("{pas du json", "x");
        CHECK(!bad.error.empty());
        // Même illisible, le plugin doit se nommer : l'interface affiche
        // l'erreur à côté du nom, et une erreur anonyme n'aide personne.
        CHECK_EQ(bad.name, std::string("x"));
    }
    {
        const auto none = plugins::parse_manifest(R"({"name":"X"})", "x");
        CHECK(!none.error.empty()); // aucune action
    }
    {
        // Une ligne de shell déguisée en programme : refusée franchement,
        // plutôt qu'exécutée par un chemin détourné.
        const auto sh = plugins::parse_manifest(
            R"({"actions":[{"label":"X","run":"rm -rf / && echo"}]})", "x");
        CHECK(!sh.error.empty());
        CHECK(sh.actions.empty());
    }
    {
        // Action sans « run » : ignorée, et si c'est la seule, le plugin
        // le signale au lieu d'apparaître vide.
        const auto p = plugins::parse_manifest(
            R"({"actions":[{"label":"X"}]})", "x");
        CHECK(!p.error.empty());
    }

    // =====================================================================
    // 3. Substitution
    // =====================================================================
    {
        plugins::Context c;
        c.instanceId = "abc";
        c.instanceName = "Ma partie";
        c.instanceDir = "C:/jeux/abc";
        c.dataDir = "C:/data";
        CHECK_EQ(plugins::expand("{instanceDir}/mods", c),
                 std::string("C:/jeux/abc/mods"));
        CHECK_EQ(plugins::expand("{instanceId}-{instanceId}", c),
                 std::string("abc-abc"));
        CHECK_EQ(plugins::expand("rien a remplacer", c),
                 std::string("rien a remplacer"));
        // Clé non renseignée : le substituable RESTE en clair. Le vider
        // transformerait « {instanceDir}/mods » en « /mods », qui désigne
        // un dossier bien réel et pas celui-là.
        CHECK_EQ(plugins::expand("{gameVersion}/x", c),
                 std::string("{gameVersion}/x"));

        plugins::Action a;
        a.program = "explorer";
        a.args = {"{instanceDir}/mods", "avec espace"};
        const std::string pv = plugins::preview(a, c);
        CHECK(pv.find("C:/jeux/abc/mods") != std::string::npos);
        CHECK(pv.find("\"avec espace\"") != std::string::npos);
    }

    // =====================================================================
    // 4. Autorisation : liée à l'empreinte du manifeste
    // =====================================================================
    {
        const fs::path root = plugins::dir();
        write_plugin(root, "demo",
                     R"({"name":"Demo","actions":[{"label":"A","run":"echo"}]})");
        auto all = plugins::list();
        CHECK_EQ(all.size(), size_t{1});
        if (!all.empty()) {
        CHECK(!all[0].enabled);

        // Refus tant que ce n'est pas autorisé — y compris en appelant
        // run() directement, et pas seulement via l'interface.
        {
            std::string err;
            CHECK(!plugins::run(all[0], all[0].actions[0], {}, &err));
            CHECK(!err.empty());
        }

        plugins::set_enabled(all[0], true);
        all = plugins::list();
        CHECK(all[0].enabled);

        // Le manifeste change : l'autorisation tombe. Sans cela, autoriser
        // une fois reviendrait à signer un chèque en blanc pour tout ce
        // que le fichier dirait ensuite.
        write_plugin(root, "demo",
                     R"({"name":"Demo","actions":[{"label":"A","run":"autre"}]})");
        all = plugins::list();
        CHECK_EQ(all.size(), size_t{1});
        CHECK(!all[0].enabled);

        // Revenir EXACTEMENT au manifeste approuvé le réautorise, et
        // c'est voulu : l'autorisation porte sur un contenu, et ce
        // contenu est précisément celui que l'utilisateur a lu puis
        // accepté. Redemander son accord pour des octets identiques
        // serait du bruit, pas de la sécurité.
        write_plugin(root, "demo",
                     R"({"name":"Demo","actions":[{"label":"A","run":"echo"}]})");
        all = plugins::list();
        CHECK(all[0].enabled);

        // Retirer l'autorisation, en revanche, efface TOUTES les
        // empreintes de ce plugin : revenir en arrière ne doit alors
        // rien ressusciter.
        plugins::set_enabled(all[0], false);
        write_plugin(root, "demo",
                     R"({"name":"Demo","actions":[{"label":"A","run":"autre"}]})");
        all = plugins::list();
        CHECK(!all[0].enabled);
        write_plugin(root, "demo",
                     R"({"name":"Demo","actions":[{"label":"A","run":"echo"}]})");
        all = plugins::list();
        CHECK(!all[0].enabled);
        }
    }

    // =====================================================================
    // 5. API HTTP locale
    // =====================================================================
    {
        // Sans jeton, elle ne démarre pas du tout.
        std::string err;
        CHECK(!localapi::start(27931, "", &err));
        CHECK(!err.empty());
        CHECK(!localapi::running());

        const std::string token = localapi::make_token();
        CHECK(token.size() >= 32);
        // Aléatoire, pas une constante : deux jetons ne se ressemblent pas.
        CHECK(localapi::make_token() != token);

        // Port au hasard dans la plage éphémère : un test ne doit pas
        // échouer parce qu'un autre programme occupait un port fixe.
        int port = 0;
        bool up = false;
        for (int p = 27931; p < 27960 && !up; ++p) {
            up = localapi::start(p, token, &err);
            if (up) port = p;
        }
        CHECK(up);
        if (!up)
            std::printf("INFO API locale non démarrable : %s\n", err.c_str());
        if (up) {
        CHECK(localapi::running());
        CHECK_EQ(localapi::port(), port);

        localapi::publish(R"({"state":"idle"})", R"([{"id":"a"}])");

        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(3, 0);

        // Sans jeton : 401, et surtout AUCUNE donnée.
        if (auto r = cli.Get("/api/status")) {
            CHECK_EQ(r->status, 401);
            CHECK(r->body.find("idle") == std::string::npos);
        } else {
            CHECK(false);
        }
        // Mauvais jeton : 401 aussi.
        if (auto r = cli.Get("/api/status",
                             {{"Authorization", "Bearer faux"}})) {
            CHECK_EQ(r->status, 401);
        } else {
            CHECK(false);
        }
        // Bon jeton : l'instantané publié, tel quel.
        const httplib::Headers auth{{"Authorization", "Bearer " + token}};
        if (auto r = cli.Get("/api/status", auth)) {
            CHECK_EQ(r->status, 200);
            CHECK(r->body.find("idle") != std::string::npos);
        } else {
            CHECK(false);
        }
        if (auto r = cli.Get("/api/instances", auth)) {
            CHECK_EQ(r->status, 200);
            CHECK(r->body.find("\"a\"") != std::string::npos);
        } else {
            CHECK(false);
        }
        // Inconnu : 404, pas une page d'erreur bavarde.
        if (auto r = cli.Get("/api/secret", auth)) CHECK_EQ(r->status, 404);

        // Lancement : déposé dans la file, jamais exécuté par ce fil.
        CHECK(localapi::take_launch_requests().empty());
        if (auto r = cli.Post("/api/launch", auth, R"({"id":"monid"})",
                              "application/json")) {
            CHECK_EQ(r->status, 200);
        } else {
            CHECK(false);
        }
        const auto queued = localapi::take_launch_requests();
        CHECK_EQ(queued.size(), size_t{1});
        if (!queued.empty()) CHECK_EQ(queued[0], std::string("monid"));
        // Consommée une seule fois.
        CHECK(localapi::take_launch_requests().empty());

        // Corps sans id : refusé.
        if (auto r = cli.Post("/api/launch", auth, "{}", "application/json"))
            CHECK_EQ(r->status, 400);
        // Un lancement sans jeton ne doit RIEN déposer.
        cli.Post("/api/launch", R"({"id":"pirate"})", "application/json");
        CHECK(localapi::take_launch_requests().empty());

        localapi::stop();
        CHECK(!localapi::running());
        }
    }

    // =====================================================================
    // 5 bis. Clés d'application : portées, révocation, non-conservation
    // =====================================================================
    {
        using namespace tl;
        CHECK(apikeys::list().empty());

        const auto a = apikeys::create("Bot support", "theo@exemple",
                                       {apikeys::kScopeDiag});
        CHECK(!a.secret.empty());
        CHECK(a.key.id.rfind("ak_", 0) == 0);
        // Le secret n'est PAS conservé : seule son empreinte l'est. Lire la
        // configuration ne doit pas permettre de s'authentifier.
        CHECK_EQ(a.key.hash, sha1_hex_of(a.secret));
        CHECK(DataStore::settings.apiKeys.dump().find(a.secret) ==
              std::string::npos);

        // La portée accordée passe, les autres non.
        CHECK(apikeys::check(a.secret, apikeys::kScopeDiag).ok);
        const auto denied = apikeys::check(a.secret, apikeys::kScopeControl);
        CHECK(!denied.ok);
        // Refusée pour la PORTÉE, pas pour l'authentification : l'appelant
        // doit pouvoir faire la différence.
        CHECK(denied.scopeDenied);

        // Un secret inconnu n'est ni authentifié ni « portée refusée ».
        const auto unknown = apikeys::check("pas-une-cle", apikeys::kScopeDiag);
        CHECK(!unknown.ok);
        CHECK(!unknown.scopeDenied);

        // Les appels sont comptés : sans cela on ne sait pas quoi révoquer.
        apikeys::flush();
        auto all = apikeys::list();
        CHECK_EQ(all.size(), size_t{1});
        if (!all.empty()) {
            CHECK(all[0].calls >= 1);
            CHECK(all[0].lastUsedUnix > 0);
        }

        // Une deuxième clé ne doit pas être gênée par la révocation de la
        // première — c'est tout l'intérêt d'une clé par application.
        const auto b = apikeys::create("Overlay", "", {apikeys::kScopeRead});
        CHECK(a.secret != b.secret);
        CHECK(apikeys::revoke(a.key.id));
        CHECK(!apikeys::check(a.secret, apikeys::kScopeDiag).ok);
        CHECK(apikeys::check(b.secret, apikeys::kScopeRead).ok);

        // Aucune portée demandée : on donne la plus inoffensive, pas rien.
        const auto c = apikeys::create("Sans portée", "", {});
        CHECK_EQ(c.key.scopes.size(), size_t{1});
        if (!c.key.scopes.empty())
            CHECK_EQ(c.key.scopes[0], std::string(apikeys::kScopeDiag));

        // Aller-retour JSON : ce qui sort doit pouvoir rentrer.
        const auto round = apikeys::from_json(apikeys::to_json(apikeys::list()));
        CHECK_EQ(round.size(), apikeys::list().size());

        // Nettoyage, pour ne pas gêner la section suivante.
        for (const auto& k : apikeys::list()) apikeys::remove(k.id);
        CHECK(apikeys::list().empty());
    }

    // =====================================================================
    // 5 ter. Diagnostic par HTTP : ce que l'API vend réellement
    // =====================================================================
    {
        using namespace tl;
        const auto diagKey = apikeys::create("Bot", "", {apikeys::kScopeDiag});
        const auto readKey = apikeys::create("Widget", "", {apikeys::kScopeRead});
        const std::string owner = localapi::make_token();
        int port = 0;
        bool up = false;
        std::string err;
        for (int p = 27961; p < 27990 && !up; ++p) {
            up = localapi::start(p, owner, &err);
            if (up) port = p;
        }
        CHECK(up);
        if (up) {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(3, 0);
            cli.set_read_timeout(5, 0);
            const httplib::Headers hDiag{
                {"Authorization", "Bearer " + diagKey.secret}};
            const httplib::Headers hRead{
                {"Authorization", "Bearer " + readKey.secret}};

            // Un log brut, envoyé tel quel : pas d'échappement JSON à faire
            // sur 400 Ko de texte.
            const std::string log =
                "java.lang.NullPointerException: boom\n"
                "\tat fr.webscreen.registry.ModItems.go(ModItems.java:35)\n"
                "-- System Details --\n"
                "\tMinecraft Version: 1.12.2\n"
                "\t| State | ID        | Version | Source                |\n"
                "\t| LCH   | webscreen | 2.0.0   | WebDisplay2-2.0.0.jar |\n";
            if (auto r = cli.Post("/v1/diag/crash", hDiag, log, "text/plain")) {
                CHECK_EQ(r->status, 200);
                CHECK(r->body.find("webscreen") != std::string::npos);
                CHECK(r->body.find("WebDisplay2") != std::string::npos);
                CHECK(r->body.find("mod_error") != std::string::npos);
            } else {
                CHECK(false);
            }

            // La clé « lecture » ne doit PAS pouvoir analyser : 403, et non
            // 401 — la clé est bonne, c'est le droit qui manque.
            if (auto r = cli.Post("/v1/diag/crash", hRead, log, "text/plain"))
                CHECK_EQ(r->status, 403);
            else
                CHECK(false);
            // Et réciproquement.
            if (auto r = cli.Get("/v1/status", hDiag)) CHECK_EQ(r->status, 403);

            // Analyse de mods : manifeste brut, l'appelant n'a pas à savoir
            // lire les cinq formats — c'est ce qu'il vient chercher.
            const std::string body =
                R"({"loader":"Fabric","mcVersion":"1.20.1","mods":[)"
                R"({"file":"vieux.jar","manifest":"{\"id\":\"vieux\",)"
                R"(\"version\":\"1.0\",\"depends\":{\"minecraft\":\"1.19.2\"}}"},)"
                R"({"id":"sain","version":"1.0","loader":"fabric"}]})";
            if (auto r = cli.Post("/v1/diag/mods", hDiag, body,
                                  "application/json")) {
                CHECK_EQ(r->status, 200);
                // Le mod déclaré pour 1.19.2 doit ressortir bloquant.
                CHECK(r->body.find("\"errors\":1") != std::string::npos);
                CHECK(r->body.find("vieux.jar") != std::string::npos);
            } else {
                CHECK(false);
            }
            // Corps illisible : 400, pas 500.
            if (auto r = cli.Post("/v1/diag/mods", hDiag, "{pas du json",
                                  "application/json"))
                CHECK_EQ(r->status, 400);

            // Auto-description : accessible à toute clé valable.
            if (auto r = cli.Get("/v1", hDiag)) {
                CHECK_EQ(r->status, 200);
                CHECK(r->body.find("/v1/diag/crash") != std::string::npos);
            }
            // Sans clé du tout : 401 partout.
            if (auto r = cli.Post("/v1/diag/crash", log, "text/plain"))
                CHECK_EQ(r->status, 401);

            // Le jeton du propriétaire garde tous les droits.
            const httplib::Headers hOwner{{"Authorization", "Bearer " + owner}};
            if (auto r = cli.Post("/v1/diag/crash", hOwner, log, "text/plain"))
                CHECK_EQ(r->status, 200);
            if (auto r = cli.Get("/v1/status", hOwner)) CHECK_EQ(r->status, 200);

            // Une clé révoquée est refusée immédiatement, sans redémarrage.
            apikeys::revoke(diagKey.key.id);
            if (auto r = cli.Post("/v1/diag/crash", hDiag, log, "text/plain"))
                CHECK_EQ(r->status, 401);

            // L'ancien chemin reste servi : rien de ce qui marchait hier ne
            // doit cesser de marcher.
            if (auto r = cli.Get("/api/status", hOwner)) CHECK_EQ(r->status, 200);

            localapi::stop();
        }
        for (const auto& k : apikeys::list()) apikeys::remove(k.id);
    }

    // =====================================================================
    // 5 quater. Événements sortants : jusqu'au récepteur
    // =====================================================================
    {
        using namespace tl;
        // HTTPS exigé, sauf bouclage — le corps porte le nom de
        // l'instance et le secret voyage en en-tête.
        std::string why;
        CHECK(events::url_acceptable("https://exemple.test/hook"));
        CHECK(!events::url_acceptable("http://exemple.test/hook", &why));
        CHECK(!why.empty());
        CHECK(events::url_acceptable("http://127.0.0.1:1234/hook"));
        CHECK(events::url_acceptable("http://localhost:1234/hook"));
        CHECK(!events::url_acceptable("ftp://exemple.test/hook"));
        CHECK(!events::url_acceptable(""));

        // Un récepteur, comme en vrai.
        httplib::Server recv;
        std::mutex rm;
        std::vector<std::string> received;
        std::vector<std::string> secrets;
        recv.Post("/hook", [&](const httplib::Request& rq,
                               httplib::Response& rs) {
            std::lock_guard<std::mutex> lk(rm);
            received.push_back(rq.body);
            auto it = rq.headers.find("X-TeamLauncher-Secret");
            secrets.push_back(it == rq.headers.end() ? "" : it->second);
            rs.set_content("{}", "application/json");
        });
        int rport = 0;
        std::thread rth;
        for (int p = 28001; p < 28030 && rport == 0; ++p) {
            // On ne peut pas savoir si un port est libre sans l'essayer :
            // on lance directement le vrai serveur et on vérifie.
            rth = std::thread([&recv, p] { recv.listen("127.0.0.1", p); });
            recv.wait_until_ready();
            if (recv.is_running()) {
                rport = p;
            } else {
                rth.join();
            }
        }
        CHECK(rport != 0);
        if (rport != 0) {
            const std::string url =
                "http://127.0.0.1:" + std::to_string(rport) + "/hook";
            CHECK(events::add(url, "mon-secret", {events::kCrash}));
            const auto hooks = events::list();
            CHECK_EQ(hooks.size(), size_t{1});

            // L'essai est synchrone : il doit aboutir.
            std::string terr;
            CHECK(events::test(hooks[0].id, &terr));
            if (!terr.empty()) std::printf("INFO test hook: %s\n", terr.c_str());

            // Et l'émission asynchrone, avec un diagnostic complet.
            nlohmann::json ev{
                {"exitCode", 1},
                {"instance", {{"id", "abc"}, {"name", "Ma partie"}}},
                {"diagnosis",
                 {{"cause", "mod_error"},
                  {"title", "Le crash vient du mod « webscreen »."},
                  {"suspects", nlohmann::json::array({{{"modId", "webscreen"}}})}}}};
            events::emit(events::kCrash, ev);

            // L'envoi est sur un fil de fond : on attend qu'il arrive,
            // sans jamais dormir aveuglément.
            for (int i = 0; i < 100; ++i) {
                {
                    std::lock_guard<std::mutex> lk(rm);
                    if (received.size() >= 2) break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            std::lock_guard<std::mutex> lk(rm);
            CHECK(received.size() >= 2);
            if (received.size() >= 2) {
                // Le message d'essai, puis le crash analysé.
                CHECK(received[0].find("\"event\":\"test\"") !=
                      std::string::npos);
                CHECK(received[1].find("webscreen") != std::string::npos);
                CHECK(received[1].find("mod_error") != std::string::npos);
                CHECK(received[1].find("\"event\":\"crash\"") !=
                      std::string::npos);
                // Le secret voyage en en-tête, jamais dans le corps.
                CHECK_EQ(secrets[1], std::string("mon-secret"));
                CHECK(received[1].find("mon-secret") == std::string::npos);
            }
        }
        // Un type auquel personne n'est abonné ne doit rien envoyer.
        {
            size_t before;
            {
                std::lock_guard<std::mutex> lk(rm);
                before = received.size();
            }
            events::emit(events::kGameStart, nlohmann::json::object());
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            std::lock_guard<std::mutex> lk(rm);
            CHECK_EQ(received.size(), before);
        }

        events::stop();
        recv.stop();
        if (rth.joinable()) rth.join();
        for (const auto& h : events::list()) events::remove(h.id);
    }

    // =====================================================================
    // 6. Cache du balayage Java
    // =====================================================================
    {
        forget_java_scan();
        const auto t0 = std::chrono::steady_clock::now();
        const auto a = find_java(8);
        const auto t1 = std::chrono::steady_clock::now();
        const auto b = find_java(8);
        const auto t2 = std::chrono::steady_clock::now();
        CHECK(a.has_value() == b.has_value());
        if (a && b) CHECK_EQ(*a, *b);
        const auto first = std::chrono::duration_cast<std::chrono::microseconds>(
                               t1 - t0)
                               .count();
        const auto second =
            std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1)
                .count();
        std::printf("INFO balayage Java : %lld us puis %lld us\n",
                    static_cast<long long>(first),
                    static_cast<long long>(second));
        // Le second passage lit une table : il ne doit pas refaire le
        // balayage. On ne compare pas aux durées (une machine chargée fausse
        // tout) mais à un plafond franc.
        CHECK(second < 5000);
    }

    localapi::stop();
    DataStore::shutdown();
    fs::remove_all(tmp, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

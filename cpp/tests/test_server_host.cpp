// Tests hors reseau du portage PterodactylApi.cs : construction d'URL,
// validation des hotes, parsing des reponses (listes, etat, token, fichiers,
// allocations), (de)serialisation JSON et aller-retour persistance via
// DataStore (cle "PteroHosts"). AUCUN reseau sauf garde TL_TEST_NET.

#include "server_host.hpp"

#include "datastore.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tl::ptero;

static int g_failures = 0;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failures;                                               \
        }                                                               \
    } while (0)

#define CHECK_EQ(a, b)                                                         \
    do {                                                                       \
        const auto va = (a);                                                   \
        const auto vb = (b);                                                   \
        if (!(va == vb)) {                                                     \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_THROWS(expr)                                                \
    do {                                                                  \
        bool thrown = false;                                              \
        try {                                                             \
            expr;                                                         \
        } catch (...) {                                                   \
            thrown = true;                                                \
        }                                                                 \
        if (!thrown) {                                                    \
            std::printf("FAIL %s:%d: %s did not throw\n", __FILE__,        \
                        __LINE__, #expr);                                 \
            ++g_failures;                                                 \
        }                                                                 \
    } while (0)

static Host make_host() {
    return Host{"Mon serveur", "https://panel.exemple.fr", "ptlc_cle-de-test",
                "a1b2c3d4"};
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string out;
    char buf[4096];
    while (in.read(buf, sizeof(buf)) || in.gcount() > 0)
        out.append(buf, static_cast<size_t>(in.gcount()));
    return out;
}

int main() {
    const Host h = make_host();

    // --- 1. Construction d'URL (PanelUrl + "/api/client" + path, C#) ---
    CHECK_EQ(api_url(h, "/servers"),
             std::string("https://panel.exemple.fr/api/client/servers"));
    CHECK_EQ(api_url(h, "/servers/a1b2c3d4/resources"),
             std::string("https://panel.exemple.fr/api/client/servers/"
                         "a1b2c3d4/resources"));
    Host slashed = h;
    slashed.panelUrl = "https://panel.exemple.fr///";
    CHECK_EQ(api_url(slashed, "/servers"),
             std::string("https://panel.exemple.fr/api/client/servers"));
    CHECK_EQ(auth_headers(h), std::string("Authorization: Bearer ptlc_cle-de-test\r\n"
                                          "Accept: application/json"));
    CHECK_EQ(detail::url_encode("/"), std::string("%2F"));
    CHECK_EQ(detail::url_encode("a b"), std::string("a%20b"));

    // --- 2. Validation des hotes (refus des entrees invalides, sans reseau) ---
    CHECK(valid_host(h, nullptr));
    std::string err;
    CHECK(valid_host(h, &err) && err.empty());
    auto expect_invalid = [&](Host bad) {
        std::string e;
        CHECK(!valid_host(bad, &e));
        CHECK(!e.empty());
    };
    {
        Host bad = h;
        bad.name = "   ";
        expect_invalid(bad);
    }
    {
        Host bad = h;
        bad.panelUrl = "ftp://panel.exemple.fr";
        expect_invalid(bad);
        bad.panelUrl = "panel.exemple.fr";
        expect_invalid(bad); // scheme obligatoire
        bad.panelUrl = "https://";
        expect_invalid(bad); // hote manquant
        bad.panelUrl = "";
        expect_invalid(bad);
        bad.panelUrl = "https://panel avec espace.fr";
        expect_invalid(bad);
    }
    {
        Host bad = h;
        bad.apiKey = "";
        expect_invalid(bad);
    }
    {
        Host bad = h;
        bad.serverId = "";
        expect_invalid(bad);
        bad.serverId = "a/b";
        expect_invalid(bad); // anti-injection de chemin
        bad.serverId = "a b";
        expect_invalid(bad);
        bad.serverId = "../x";
        expect_invalid(bad);
        bad.serverId = std::string(65, 'a');
        expect_invalid(bad);
        bad.serverId = "a1b2-c3_d4"; // tiret/underscore OK
        CHECK(valid_host(bad, nullptr));
    }

    // --- 3. Parsing : liste des serveurs ---
    const char* kList = R"({
        "data": [
            {"attributes": {"identifier": "a1b2c3d4", "name": "Survie",
                "node": "VPS", "status": "running", "cpu": 100,
                "memory": 2048, "disk": 10240,
                "allocations": [{"port": 25565}, {"port": 25566}]}},
            {"attributes": {"identifier": "e5f6g7h8", "name": "Créatif"}}
        ]})";
    {
        const auto v = parse_server_list(kList);
        CHECK_EQ(static_cast<int>(v.size()), 2);
        CHECK_EQ(v[0].id, std::string("a1b2c3d4"));
        CHECK_EQ(v[0].name, std::string("Survie"));
        CHECK_EQ(v[0].node, std::string("VPS"));
        CHECK_EQ(v[0].status, std::string("running"));
        CHECK_EQ(v[0].cpu, 100);
        CHECK_EQ(v[0].memBytes, 2048LL);
        CHECK_EQ(v[0].diskBytes, 10240LL);
        CHECK_EQ(static_cast<int>(v[0].allocations.size()), 2);
        CHECK_EQ(v[0].allocations[0], 25565);
        // Champs optionnels absents -> defauts C#.
        CHECK_EQ(v[1].node, std::string(""));
        CHECK_EQ(v[1].status, std::string("unknown"));
        CHECK_EQ(v[1].cpu, 0);
        CHECK(v[1].allocations.empty());
    }
    CHECK(parse_server_list(R"({"data": []})").empty());
    CHECK(parse_server_list(R"({})").empty()); // sans "data" -> vide (C#)
    CHECK_THROWS(parse_server_list("{ pas du json"));
    CHECK_THROWS(parse_server_list("[1,2]"));

    // --- 4. Parsing : etat / utilisation ---
    const char* kState = R"({
        "attributes": {"running": true, "installing": false,
            "cpu_absolute": 12.7, "memory_bytes": 536870912,
            "disk_bytes": 1073741824, "uptime": 3600,
            "state": {"players": 3, "max_players": 20}}})";
    {
        const PtServerState st = parse_server_state(kState);
        CHECK(st.isRunning);
        CHECK(!st.isInstalling);
        CHECK_EQ(st.cpuPercent, 12); // (int)GetDouble
        CHECK_EQ(st.memUsedBytes, 536870912LL);
        CHECK_EQ(st.diskUsedBytes, 1073741824LL);
        CHECK_EQ(st.uptimeSeconds, 3600);
        CHECK_EQ(st.players, 3);
        CHECK_EQ(st.maxPlayers, 20);
    }
    {
        // Sans "attributes" -> etat par defaut (arrete), comme le C#.
        const PtServerState st = parse_server_state(R"({})");
        CHECK(!st.isRunning);
        CHECK_EQ(st.cpuPercent, 0);
    }
    CHECK_THROWS(parse_server_state("{ pas du json"));

    // --- 5. Parsing : token websocket ---
    {
        const auto tok =
            parse_websocket_token(R"({"data": {"token": "abc123"}})");
        CHECK(tok.has_value());
        CHECK_EQ(tok.value_or(""), std::string("abc123"));
    }
    CHECK(!parse_websocket_token(R"({"errors": []})").has_value());
    CHECK(!parse_websocket_token(R"({})").has_value());

    // --- 6. Parsing : fichiers + allocations ---
    const char* kFiles = R"({
        "data": [
            {"attributes": {"name": "server.jar", "is_file": true,
                "size": 12345, "mime_type": "application/java-archive",
                "modified_at": "2026-01-02T03:04:05+00:00"}},
            {"attributes": {"name": "world", "is_file": false, "size": 0,
                "mime_type": "inode/directory",
                "modified_at": "2026-01-01T00:00:00+00:00"}}]})";
    {
        const auto v = parse_file_list(kFiles);
        CHECK_EQ(static_cast<int>(v.size()), 2);
        CHECK_EQ(v[0].name, std::string("server.jar"));
        CHECK(!v[0].isDirectory); // is_file=true -> pas un dossier
        CHECK_EQ(v[0].size, 12345LL);
        CHECK_EQ(v[0].mimeType, std::string("application/java-archive"));
        CHECK_EQ(v[0].modified, std::string("2026-01-02T03:04:05+00:00"));
        CHECK(v[1].isDirectory);
    }
    {
        const auto v = parse_allocations(
            R"({"data": [{"attributes": {"ip": "51.255.207.183",
                                         "port": 25565}}]})");
        CHECK_EQ(static_cast<int>(v.size()), 1);
        CHECK_EQ(v[0].ip, std::string("51.255.207.183"));
        CHECK_EQ(v[0].port, 25565);
    }
    CHECK_THROWS(parse_file_list("{ pas du json"));
    CHECK_THROWS(parse_allocations("{ pas du json"));

    // --- 7. Refus avant reseau : signal inconnu / hote invalide ---
    CHECK_THROWS(power(h, "boom")); // invalid_argument, aucun HTTP
    CHECK_THROWS(power(h, ""));
    {
        Host bad = h;
        bad.apiKey = "";
        CHECK_THROWS(power(bad, "start")); // hote refuse avant tout reseau
        CHECK_THROWS(send_command(bad, "list"));
        CHECK_THROWS(list_servers(bad));
        CHECK_THROWS(list_files(bad));
        CHECK_THROWS(allocations(bad));
        CHECK_THROWS(delete_files(bad, "/", {"x"}));
    }
    // server_state / websocket_token ne levent jamais (defaut/null, C#).
    {
        Host unreachable{"X", "http://127.0.0.1:9", "k", "a1b2c3d4"};
        const PtServerState st = server_state(unreachable);
        CHECK(!st.isRunning);
        CHECK(!websocket_token(unreachable).has_value());
    }

    // --- 8. (De)serialisation JSON ---
    {
        const json j = host_to_json(h);
        CHECK_EQ(j.value("Name", ""), h.name);
        CHECK_EQ(j.value("PanelUrl", ""), h.panelUrl);
        CHECK_EQ(j.value("ApiKey", ""), h.apiKey);
        CHECK_EQ(j.value("ServerId", ""), h.serverId);
        CHECK(host_from_json(j) == h);
        CHECK(!(host_from_json(j) != h));
    }
    CHECK_THROWS(host_from_json(json::array()));
    CHECK_THROWS(host_from_json(json{{"Name", "x"}})); // champs manquants

    // --- 9. Persistance : aller-retour via DataStore (dossier jetable) ---
    const fs::path tmp = fs::temp_directory_path() / "tl-server-host-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#else
    setenv("TL_DATA_DIR", tmp.string().c_str(), 1);
#endif
    tl::DataStore::settings = tl::AppSettings{};
    tl::DataStore::load();

    Host h1{"Survie", "https://panel.exemple.fr/", "cle-1", "a1b2c3d4"};
    add_host(h1); // '/' final rogne
    add_host(Host{"Créatif", "https://panel.exemple.fr", "cle-2", "e5f6g7h8"});
    {
        const auto v = load_hosts();
        CHECK_EQ(static_cast<int>(v.size()), 2);
        CHECK_EQ(v[0].panelUrl, std::string("https://panel.exemple.fr"));
        CHECK_EQ(v[0].apiKey, std::string("cle-1"));
        CHECK_EQ(v[1].name, std::string("Créatif"));
    }
    CHECK_THROWS(add_host(Host{"", "https://panel.exemple.fr", "k", "zz"}));

    // Re-configuration a (url,id) egaux : remplace, sans doublon.
    add_host(Host{"Survie v2", "https://panel.exemple.fr", "cle-1b", "a1b2c3d4"});
    CHECK_EQ(static_cast<int>(load_hosts().size()), 2);
    CHECK_EQ(load_hosts()[0].name, std::string("Survie v2"));

    // Entree corrompue : ignoree, les autres conservees.
    tl::DataStore::settings.pteroHosts.push_back(json{{"Name", "x"}});
    CHECK_EQ(static_cast<int>(load_hosts().size()), 2);

    tl::DataStore::saveNow();
    const fs::path cfg = tmp / "config.json";
    CHECK(fs::exists(cfg));
    CHECK(read_file(cfg).find("\"PteroHosts\"") != std::string::npos);

    tl::DataStore::settings = tl::AppSettings{};
    tl::DataStore::load();
    {
        const auto v = load_hosts();
        CHECK_EQ(static_cast<int>(v.size()), 2);
        CHECK_EQ(v[0].serverId, std::string("a1b2c3d4"));
        CHECK_EQ(v[0].apiKey, std::string("cle-1b"));
    }
    CHECK(remove_host("https://panel.exemple.fr/", "a1b2c3d4"));
    CHECK(!remove_host("https://panel.exemple.fr", "a1b2c3d4")); // deja parti
    CHECK(!remove_host("https://autre.fr", "nope"));
    tl::DataStore::saveNow();
    tl::DataStore::settings = tl::AppSettings{};
    tl::DataStore::load();
    CHECK_EQ(static_cast<int>(load_hosts().size()), 1);

    // --- 10. Reseau reel (TL_TEST_NET=1 + TL_PTERO_URL/KEY/SID) ---
    if (const char* net = std::getenv("TL_TEST_NET"); net && *net) {
        const char* url = std::getenv("TL_PTERO_URL");
        const char* key = std::getenv("TL_PTERO_KEY");
        const char* sid = std::getenv("TL_PTERO_SID");
        if (url && key && sid && *url && *key && *sid) {
            try {
                Host live{"live", url, key, sid};
                const auto v = list_servers(live);
                std::printf("INFO reseau : %d serveur(s) listes\n",
                            static_cast<int>(v.size()));
                const PtServerState st = server_state(live);
                std::printf("INFO reseau : running=%d cpu=%d\n",
                            st.isRunning ? 1 : 0, st.cpuPercent);
            } catch (const std::exception& e) {
                std::printf("INFO reseau : echec (%s)\n", e.what());
            }
        } else {
            std::printf("INFO test reseau saute (TL_PTERO_URL/KEY/SID manquants)\n");
        }
    } else {
        std::printf("INFO test reseau saute (TL_TEST_NET non defini)\n");
    }

    tl::DataStore::shutdown();
    fs::remove_all(tmp, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

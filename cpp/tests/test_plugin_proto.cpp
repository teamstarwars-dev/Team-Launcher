// Protocole IPC des plugins (V8).
//
// Ce que ce protocole doit garantir tient en un mot : refuser. Un plugin
// est du code tiers ; tout ce qu'il envoie est supposé hostile. Ces tests
// portent donc moins sur ce qui passe que sur ce qui ne doit pas passer.

#include "plugin_proto.hpp"

#include <cstdio>
#include <string>

using namespace tl::pluginproto;
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
        const auto _a = (a);                                                 \
        const auto _b = (b);                                                 \
        if (!(_a == _b)) {                                                   \
            std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static const std::string kId(32, 'a'); // identifiant d'instance valide

// Rend le code d'erreur, ou 0 si la requête est acceptée.
static int code_of(const std::string& frame) {
    Error e;
    return parse_request(frame, &e) ? 0 : e.code;
}

int main() {
    // =====================================================================
    // 1. Trames
    // =====================================================================
    {
        FrameReader r;
        const std::string a = encode_frame("{\"x\":1}");
        const std::string b = encode_frame("{\"y\":2}");
        // Deux trames d'un coup.
        r.feed((a + b).data(), a.size() + b.size());
        std::string out;
        CHECK(r.next(&out) == FrameReader::Status::Frame);
        CHECK_EQ(out, std::string("{\"x\":1}"));
        CHECK(r.next(&out) == FrameReader::Status::Frame);
        CHECK_EQ(out, std::string("{\"y\":2}"));
        CHECK(r.next(&out) == FrameReader::Status::NeedMore);
    }
    {
        // Octet par octet : le découpage réseau ne doit rien changer.
        FrameReader r;
        const std::string a = encode_frame("{\"lent\":true}");
        std::string out;
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(r.next(&out) == FrameReader::Status::NeedMore);
            r.feed(&a[i], 1);
        }
        CHECK(r.next(&out) == FrameReader::Status::Frame);
        CHECK_EQ(out, std::string("{\"lent\":true}"));
    }
    {
        // Longueur nulle : invalide, et le lecteur reste empoisonné.
        FrameReader r;
        const char zero[4] = {0, 0, 0, 0};
        r.feed(zero, 4);
        std::string out;
        CHECK(r.next(&out) == FrameReader::Status::Invalid);
        CHECK(r.poisoned());
        // Même une trame parfaite, ensuite, n'est plus lue : après une
        // longueur mensongère, on ne sait plus où commence la suivante.
        const std::string ok = encode_frame("{}");
        r.feed(ok.data(), ok.size());
        CHECK(r.next(&out) == FrameReader::Status::Invalid);
    }
    {
        // Longueur annoncée au-delà du plafond.
        FrameReader r;
        const char big[4] = {0, 0, 0x10, 0}; // 1 Mio
        r.feed(big, 4);
        std::string out;
        CHECK(r.next(&out) == FrameReader::Status::Invalid);
        CHECK(r.poisoned());
    }
    {
        // Inondation sans jamais finir de trame : le tampon est borné.
        FrameReader r;
        const char hdr[4] = {static_cast<char>(0xFF), static_cast<char>(0xFF), 0, 0};
        r.feed(hdr, 4); // annonce 65535 octets
        const std::string chunk(32 * 1024, 'x');
        for (int i = 0; i < 8; ++i) r.feed(chunk.data(), chunk.size());
        std::string out;
        CHECK(r.next(&out) == FrameReader::Status::Invalid);
    }

    // =====================================================================
    // 2. Requêtes acceptées
    // =====================================================================
    {
        Error e;
        auto r = parse_request(
            R"({"v":1,"id":7,"method":"instances.list","params":{}})", &e);
        CHECK(r.has_value());
        if (r) {
            CHECK_EQ(r->id, std::int64_t{7});
            CHECK_EQ(r->method, std::string("instances.list"));
        }
        // params est facultatif quand il est vide.
        CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.list"})"), 0);
        CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"host.hello","params":{"api":1}})"), 0);
        CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"log.write","params":)"
                         R"({"level":"warn","message":"ligne\tavec\ttabs"}})"),
                 0);
        CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.get","params":{"instanceId":")" +
                         kId + "\"}}"),
                 0);
        CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"ui.notify","params":)"
                         R"({"title":"Salut","message":"Bonjour"}})"),
                 0);
    }

    // =====================================================================
    // 3. Requêtes refusées
    // =====================================================================
    // Forme
    CHECK_EQ(code_of("pas du json"), kParseError);
    CHECK_EQ(code_of("[1,2,3]"), kInvalidRequest);
    CHECK_EQ(code_of(R"({"v":1,"method":"instances.list"})"), kInvalidRequest); // sans id
    CHECK_EQ(code_of(R"({"v":1,"id":-1,"method":"instances.list"})"), kInvalidRequest);
    CHECK_EQ(code_of(R"({"v":1,"id":9999999999,"method":"instances.list"})"), kInvalidRequest);
    CHECK_EQ(code_of(R"({"v":1,"id":1.5,"method":"instances.list"})"), kInvalidRequest);
    CHECK_EQ(code_of(R"({"v":2,"id":1,"method":"instances.list"})"), kInvalidRequest);
    // Clé en trop : on ne tolère pas ce qu'on n'a pas prévu.
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.list","admin":true})"),
             kInvalidRequest);
    // Méthode hors table, y compris une qui « ressemblerait » à une API.
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"accounts.token"})"), kMethodNotFound);
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"INSTANCES.LIST"})"), kMethodNotFound);

    // Paramètres
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.list","params":[]})"),
             kInvalidParams);
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.list","params":{"x":1}})"),
             kInvalidParams);
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.get","params":{"instanceId":"../../etc"}})"),
             kInvalidParams);
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"instances.get","params":{"instanceId":")" +
                     std::string(32, 'A') + "\"}}"),
             kInvalidParams); // majuscules : ce n'est pas un id du launcher
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"log.write","params":{"level":"debug","message":"x"}})"),
             kInvalidParams);
    // Séquence d'échappement terminal : sert à maquiller un journal.
    CHECK_EQ(code_of("{\"v\":1,\"id\":1,\"method\":\"log.write\",\"params\":"
                     "{\"level\":\"info\",\"message\":\"\\u001b[2Kfaux\"}}"),
             kInvalidParams);
    CHECK_EQ(code_of("{\"v\":1,\"id\":1,\"method\":\"log.write\",\"params\":"
                     "{\"level\":\"info\",\"message\":\"\\r\"}}"),
             kInvalidParams);
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"ui.notify","params":{"title":")" +
                     std::string(81, 'x') + R"(","message":"m"}})"),
             kInvalidParams); // titre trop long
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"ui.notify","params":{"title":"","message":"m"}})"),
             kInvalidParams); // titre vide
    CHECK_EQ(code_of(R"({"v":1,"id":1,"method":"host.hello","params":{"api":"1"}})"),
             kInvalidParams);

    // Imbrication : refusée AVANT l'analyse.
    {
        std::string deep = R"({"v":1,"id":1,"method":"instances.list","params":)";
        for (int i = 0; i < 40; ++i) deep += "{\"a\":";
        deep += "1";
        for (int i = 0; i < 40; ++i) deep += "}";
        deep += "}";
        CHECK_EQ(code_of(deep), kParseError);
        // Les crochets DANS une chaîne ne comptent pas.
        CHECK(json_depth(R"({"m":"[[[[[[[[[[[[[[[[[[[[[[[[[[[["})") == 1);
        CHECK(json_depth(R"({"m":"\"[[["})") == 1); // guillemet échappé
    }

    // UTF-8 invalide : refusé à l'entrée plutôt que de casser à la sortie.
    {
        std::string bad = "{\"v\":1,\"id\":1,\"method\":\"log.write\",\"params\":"
                          "{\"level\":\"info\",\"message\":\"\xC3\x28\"}}";
        CHECK(code_of(bad) != 0);
    }

    // =====================================================================
    // 4. Permissions et réponses
    // =====================================================================
    CHECK_EQ(*required_permission("host.hello"), std::string(""));
    CHECK_EQ(*required_permission("log.write"), std::string(""));
    CHECK_EQ(*required_permission("instances.list"), std::string("instances:read"));
    CHECK_EQ(*required_permission("game.launch"), std::string("game:launch"));
    CHECK(!required_permission("accounts.token").has_value());
    CHECK(is_known_permission("ui:notify"));
    CHECK(!is_known_permission("accounts:read"));
    CHECK(!is_known_permission(""));
    // Toute méthode soumise à permission vise une permission déclarable.
    for (const char* m : {"instances.list", "instances.get", "mods.list", "ui.notify",
                          "game.launch"})
        CHECK(is_known_permission(*required_permission(m)));

    {
        const json ok = json::parse(make_result(3, json{{"n", 2}}));
        CHECK_EQ(ok["id"].get<int>(), 3);
        CHECK_EQ(ok["result"]["n"].get<int>(), 2);
        // Erreur sans id lisible : pas de champ id du tout, plutôt qu'un
        // id inventé.
        const json er = json::parse(make_error(Error{kParseError, "x", -1}));
        CHECK(!er.contains("id"));
        CHECK_EQ(er["error"]["code"].get<int>(), static_cast<int>(kParseError));
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

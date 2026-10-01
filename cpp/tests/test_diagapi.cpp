// La couche partagée entre le launcher et le service autonome.
//
// Elle existe pour qu'il n'y ait QU'UNE implémentation des deux analyses.
// Ces tests valent donc pour les deux programmes à la fois : si le
// service hébergé et le launcher répondaient un jour différemment à la
// même question, ce serait ici que ça se verrait.

#include "diagapi.hpp"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <string>

using nlohmann::json;
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

// Une réponse qui ne se parse pas est aussi grave qu'un mauvais code :
// l'appelant recevrait du texte là où il attend du JSON.
static void check_is_json(const std::string& body, const char* what) {
    try {
        (void)json::parse(body);
    } catch (const std::exception&) {
        std::printf("FAIL réponse non-JSON pour : %s\n", what);
        ++g_failures;
    }
}

static const char* kCrashLog =
    "java.lang.NullPointerException: boom\n"
    "\tat fr.webscreen.registry.ModItems.go(ModItems.java:35)\n"
    "-- System Details --\n"
    "\tMinecraft Version: 1.12.2\n"
    "\t| State | ID        | Version | Source                |\n"
    "\t| LCH   | webscreen | 2.0.0   | WebDisplay2-2.0.0.jar |\n";

int main() {
    // =====================================================================
    // 1. Crash : log brut
    // =====================================================================
    {
        const auto r = diagapi::crash(kCrashLog);
        CHECK_EQ(r.status, 200);
        const auto j = json::parse(r.body);
        CHECK(j["found"].get<bool>());
        CHECK_EQ(j["cause"].get<std::string>(), std::string("mod_error"));
        CHECK_EQ(j["mcVersion"].get<std::string>(), std::string("1.12.2"));
        CHECK_EQ(j["suspects"].size(), size_t{1});
        if (!j["suspects"].empty()) {
            CHECK_EQ(j["suspects"][0]["modId"].get<std::string>(),
                     std::string("webscreen"));
            CHECK_EQ(j["suspects"][0]["source"].get<std::string>(),
                     std::string("WebDisplay2-2.0.0.jar"));
        }
        // Tous les champs documentés doivent être présents, même vides :
        // un client qui lit `j.loader` ne doit pas tomber sur un absent.
        for (const char* k :
             {"found", "cause", "title", "action", "loader", "mcVersion",
              "exception", "summary", "suspects"})
            CHECK(j.contains(k));
    }

    // =====================================================================
    // 2. Crash : enveloppe JSON, et un log qui commence par une accolade
    // =====================================================================
    {
        json wrap{{"log", kCrashLog}};
        const auto r = diagapi::crash(wrap.dump());
        CHECK_EQ(r.status, 200);
        CHECK(r.body.find("webscreen") != std::string::npos);
    }
    {
        // Un log peut commencer par « { » sans être du JSON. On ne doit ni
        // lancer, ni le prendre pour une enveloppe vide.
        const auto r = diagapi::crash("{ceci n'est pas du json\nOutOfMemoryError");
        CHECK_EQ(r.status, 200);
        const auto j = json::parse(r.body);
        CHECK_EQ(j["cause"].get<std::string>(), std::string("out_of_memory"));
    }
    {
        // Corps vide : 200 avec found=false, pas une erreur. Rien à
        // analyser n'est pas une faute de l'appelant.
        const auto r = diagapi::crash("");
        CHECK_EQ(r.status, 200);
        CHECK(!json::parse(r.body)["found"].get<bool>());
    }

    // =====================================================================
    // 3. Mods : manifeste brut, les cinq formats devinés
    // =====================================================================
    {
        json in{{"loader", "Fabric"},
                {"mcVersion", "1.20.1"},
                {"mods", json::array({
                             {{"file", "vieux.jar"},
                              {"manifest",
                               R"({"id":"vieux","version":"1.0",)"
                               R"("depends":{"minecraft":"1.19.2"}})"}},
                         })}};
        const auto r = diagapi::mods(in.dump());
        CHECK_EQ(r.status, 200);
        const auto j = json::parse(r.body);
        CHECK_EQ(j["errors"].get<int>(), 1);
        CHECK_EQ(j["issues"].size(), size_t{1});
        if (!j["issues"].empty()) {
            // La clé est stable et en anglais ; le libellé est du texte.
            CHECK_EQ(j["issues"][0]["severity"].get<std::string>(),
                     std::string("error"));
            CHECK(j["issues"][0].contains("severityLabel"));
            CHECK_EQ(j["issues"][0]["file"].get<std::string>(),
                     std::string("vieux.jar"));
        }
    }
    {
        // mods.toml deviné sans « format », et NeoForge distingué de Forge.
        json in{{"loader", "NeoForge"},
                {"mcVersion", "1.21"},
                {"mods", json::array({
                             {{"file", "x.jar"},
                              {"manifest",
                               "[[mods]]\nmodId=\"x\"\nversion=\"1\"\n"}},
                         })}};
        const auto r = diagapi::mods(in.dump());
        CHECK_EQ(r.status, 200);
        CHECK_EQ(json::parse(r.body)["errors"].get<int>(), 0);
    }

    // =====================================================================
    // 4. Mods : descripteurs déjà analysés
    // =====================================================================
    {
        json in{{"loader", "Fabric"},
                {"mcVersion", "1.20.1"},
                {"mods",
                 json::array(
                     {{{"id", "a"}, {"version", "1"}, {"loader", "fabric"},
                       {"deps", json::array({{{"id", "absent"},
                                              {"mandatory", true}}})}},
                      {{"id", "b"},
                       {"version", "1"},
                       {"loader", "fabric"},
                       {"deps", json::array({{{"id", "iris"},
                                              {"mandatory", false}}})}}})}};
        const auto r = diagapi::mods(in.dump());
        const auto j = json::parse(r.body);
        CHECK_EQ(j["errors"].get<int>(), 1); // dépendance obligatoire absente
        // La dépendance FACULTATIVE absente n'est pas un problème : c'est
        // une suggestion.
        CHECK_EQ(j["suggestions"].size(), size_t{1});
        if (!j["suggestions"].empty())
            CHECK_EQ(j["suggestions"][0].get<std::string>(),
                     std::string("iris"));
    }

    // =====================================================================
    // 5. Erreurs : 400 franc, jamais 500, et jamais d'exception
    // =====================================================================
    {
        CHECK_EQ(diagapi::mods("{pas du json").status, 400);
        CHECK_EQ(diagapi::mods("[]").status, 400);          // pas un objet
        CHECK_EQ(diagapi::mods(R"({"loader":"Fabric"})").status, 400); // sans mods
        // `loader` absent : 400 explicite, et surtout PAS un diagnostic
        // « aucun chargeur sur cette instance », juste pour le launcher
        // mais trompeur pour un appelant qui a oublié le champ.
        const auto r = diagapi::mods(R"({"mods":[]})");
        CHECK_EQ(r.status, 400);
        CHECK(r.body.find("loader") != std::string::npos);
        // Toute réponse, même en erreur, est du JSON : un appelant ne doit
        // jamais avoir à lire du HTML.
        for (const char* body :
             {"{pas du json", "[]", R"({"mods":[]})", ""})
            check_is_json(diagapi::mods(body).body, body);
    }

    // =====================================================================
    // 6. Auto-description
    // =====================================================================
    {
        const auto j = json::parse(diagapi::describe());
        CHECK_EQ(j["version"].get<std::string>(), std::string("1"));
        CHECK_EQ(j["endpoints"].size(), size_t{5});
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

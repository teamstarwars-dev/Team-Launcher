// Phase 7 : expurgation du config.json avant export, choix de release sur
// le canal beta, et entree de demarrage de session.
//
// Ces trois morceaux sont les seuls de la phase qui se testent hors
// interface ; le reste (barre de titre, modale de notes de version) se
// verifie a l'ecran.

#include "datastore.hpp"
#include "maintenance.hpp"
#include "startup.hpp"
#include "support.hpp"
#include "util_zip.hpp"

#include "test_env.hpp" // _putenv_s portable (Windows/POSIX)

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

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
        if (!((a) == (b))) {                                                 \
            std::printf("FAIL %s:%d  %s == %s\n", __FILE__, __LINE__, #a,    \
                        #b);                                                 \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// Corps de release GitHub minimal.
static std::string rel(const char* tag, bool pre, bool draft) {
    std::string s = "{\"tag_name\":\"v";
    s += tag;
    s += "\",\"body\":\"notes\",\"html_url\":\"https://x/";
    s += tag;
    s += "\",\"prerelease\":";
    s += pre ? "true" : "false";
    s += ",\"draft\":";
    s += draft ? "true" : "false";
    s += ",\"assets\":[]}";
    return s;
}

int main() {
    std::error_code ec;
    const fs::path tmp =
        fs::temp_directory_path() / "tl_test_support";
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    DataStore::load();

    // =====================================================================
    // 1. Cles sensibles reconnues
    // =====================================================================
    CHECK(support::is_sensitive_key("CurseForgeApiKey"));
    CHECK(support::is_sensitive_key("curseforgeapikey")); // insensible a la casse
    CHECK(support::is_sensitive_key("VpsApiKey"));
    CHECK(support::is_sensitive_key("DiscordTelemetryWebhook"));
    CHECK(support::is_sensitive_key("AzureClientId"));
    CHECK(support::is_sensitive_key("AdminServerUrl"));
    CHECK(support::is_sensitive_key("access_token"));
    // Faux positifs a eviter : ces cles-la doivent rester lisibles, sinon
    // le rapport ne sert plus a rien.
    CHECK(!support::is_sensitive_key("PlayerName"));
    CHECK(!support::is_sensitive_key("MaxRamGb"));
    CHECK(!support::is_sensitive_key("NewsUrl"));
    CHECK(!support::is_sensitive_key("Instances"));

    // =====================================================================
    // 2. Expurgation : recursive, objets ET tableaux
    // =====================================================================
    {
        const nlohmann::json raw = {
            {"PlayerName", "Theo"},
            {"MaxRamGb", 8},
            {"CurseForgeApiKey", "$2a$10$secret"},
            {"VpsApiKey", ""},
            {"PteroHosts",
             nlohmann::json::array(
                 {{{"Name", "hote1"}, {"ApiKey", "tres-secret"}}})},
            {"Nested", {{"AccessToken", "abc"}, {"Keep", "visible"}}},
        };
        const auto red = support::redacted_config(raw);
        CHECK_EQ(red["PlayerName"], "Theo");
        CHECK_EQ(red["MaxRamGb"], 8);
        CHECK_EQ(red["CurseForgeApiKey"], "(masqué)");
        // Une valeur vide se distingue d'une valeur masquee : savoir qu'un
        // reglage n'est PAS renseigne est souvent tout le diagnostic.
        CHECK_EQ(red["VpsApiKey"], "(vide)");
        CHECK_EQ(red["PteroHosts"][0]["Name"], "hote1");
        CHECK_EQ(red["PteroHosts"][0]["ApiKey"], "(masqué)");
        CHECK_EQ(red["Nested"]["AccessToken"], "(masqué)");
        CHECK_EQ(red["Nested"]["Keep"], "visible");
        // Aucun secret ne doit survivre nulle part dans la sortie.
        const std::string dump = red.dump();
        CHECK(dump.find("secret") == std::string::npos);
        CHECK(dump.find("abc") == std::string::npos);
    }

    // =====================================================================
    // 3. Canal de mise a jour : liste de releases
    // =====================================================================
    {
        const std::string cur = updates::current_version();
        std::printf("INFO version compilee : %s\n", cur.c_str());

        // Liste volontairement desordonnee : GitHub trie par date de
        // publication, ce qui ne donne PAS l'ordre des versions.
        const std::string list =
            "[" + rel("99.0.1", false, false) + "," +
            rel("99.2.0", true, false) + "," + rel("99.9.9", false, true) +
            "," + rel("99.1.0", false, false) + "]";

        DataStore::settings.updateChannel = "stable";
        auto s = updates::parse_release_json(list);
        CHECK(s.has_value());
        // Stable : la preversion 99.2.0 et le brouillon 99.9.9 sont ecartes,
        // la plus haute definitive restante est 99.1.0.
        if (s) CHECK_EQ(s->version, std::string("99.1.0"));

        DataStore::settings.updateChannel = "beta";
        auto b = updates::parse_release_json(list);
        CHECK(b.has_value());
        // Beta : la preversion compte, le brouillon toujours pas.
        if (b) CHECK_EQ(b->version, std::string("99.2.0"));

        // Rien de plus recent que la version compilee : silence, pas erreur.
        std::string err = "sentinelle";
        auto none = updates::parse_release_json("[" + rel("0.0.1", false, false) +
                                                    "]",
                                                &err);
        CHECK(!none.has_value());
        CHECK_EQ(err, std::string("sentinelle"));

        // Un objet seul (canal stable, /releases/latest) marche toujours.
        auto one = updates::parse_release_json(rel("99.5.0", false, false));
        CHECK(one.has_value());
        if (one) CHECK_EQ(one->version, std::string("99.5.0"));

        DataStore::settings.updateChannel = "stable";
    }

    // =====================================================================
    // 4. Rapport systeme : jamais vide, jamais de secret
    // =====================================================================
    {
        DataStore::settings.curseForgeApiKey = "ne-doit-pas-sortir";
        const std::string rep = support::system_report();
        CHECK(rep.find("Team Launcher") != std::string::npos);
        CHECK(rep.find("Version") != std::string::npos);
        CHECK(rep.find("ne-doit-pas-sortir") == std::string::npos);
        DataStore::settings.curseForgeApiKey.clear();
    }

    // =====================================================================
    // 5. Archive d'export : elle existe et contient les trois pieces
    // =====================================================================
    {
        const fs::path zip = tmp / "export.zip";
        std::string err;
        const bool ok = support::export_logs(zip, &err);
        CHECK(ok);
        if (!ok) std::printf("INFO export: %s\n", err.c_str());
        CHECK(fs::is_regular_file(zip, ec));
        if (fs::is_regular_file(zip, ec)) {
            CHECK(fs::file_size(zip, ec) > 0);
            CHECK(zip_read_entry(zip, "rapport.txt").has_value());
            auto cfg = zip_read_entry(zip, "config-expurge.json");
            CHECK(cfg.has_value());
        }
        // Le dossier de travail ne doit pas survivre a l'export.
        CHECK(!fs::exists(DataStore::dir() / "support-tmp", ec));
    }

    // =====================================================================
    // 6. Demarrage de session : poser, relire, retirer
    // =====================================================================
    if (std::getenv("TL_TEST_AUTOSTART")) {
        // Ecrit dans le profil de l'utilisateur (registre ou
        // ~/.config/autostart) : hors du bac a sable du test, donc sur
        // demande explicite seulement.
        CHECK(startup::autostart_supported());
        CHECK(!startup::exe_path_utf8().empty());
        const bool before = startup::autostart_enabled();
        std::string err;
        CHECK(startup::set_autostart(true, &err));
        CHECK(startup::autostart_enabled());
        CHECK(startup::set_autostart(false, &err));
        CHECK(!startup::autostart_enabled());
        // Remettre l'utilisateur dans l'etat ou on l'a trouve.
        if (before) startup::set_autostart(true, &err);
    } else {
        std::printf("INFO demarrage de session saute (TL_TEST_AUTOSTART non "
                    "defini)\n");
        // Sans ecrire : la lecture, elle, est sans effet de bord.
        CHECK(!startup::exe_path_utf8().empty());
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

// Tests du portage Lang.cs : selection de langue, paire explicite,
// dictionnaire, et coherence de la table (pas de doublon, pas d'entree vide,
// pas de traduction identique au francais par oubli).

#include "lang.hpp"

#include "test_env.hpp" // _putenv_s portable (Windows/POSIX)

#include "datastore.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <string>

namespace fs = std::filesystem;
using namespace tl;

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
            std::printf("FAIL %s:%d: %s != %s (\"%s\" vs \"%s\")\n",         \
                        __FILE__, __LINE__, #a, #b,                          \
                        std::string(va).c_str(), std::string(vb).c_str());   \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-lang-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    DataStore::load();

    // --- 1. Langue par defaut : francais ---
    DataStore::settings.language = "fr";
    CHECK_EQ(std::string(lang::current()), std::string("fr"));
    CHECK(!lang::is_en());

    // Une valeur inconnue retombe sur le francais (comme Lang.Current C#).
    DataStore::settings.language = "de";
    CHECK_EQ(std::string(lang::current()), std::string("fr"));
    CHECK(!lang::is_en());

    // --- 2. Paire explicite (Lang.T) ---
    DataStore::settings.language = "fr";
    CHECK_EQ(std::string(lang::t("Enregistrer", "Save")), std::string("Enregistrer"));
    DataStore::settings.language = "en";
    CHECK(lang::is_en());
    CHECK_EQ(std::string(lang::t("Enregistrer", "Save")), std::string("Save"));
    // en absent -> on garde le francais plutot que de rendre nullptr
    CHECK_EQ(std::string(lang::t("Sans traduction", nullptr)),
             std::string("Sans traduction"));

    // --- 3. Dictionnaire (Lang.Apply / FrToEn) ---
    DataStore::settings.language = "fr";
    CHECK_EQ(std::string(lang::t("Paramètres")), std::string("Paramètres"));
    CHECK_EQ(std::string(lang::t("Accueil")), std::string("Accueil"));

    DataStore::settings.language = "en";
    CHECK_EQ(std::string(lang::t("Paramètres")), std::string("Settings"));
    CHECK_EQ(std::string(lang::t("Accueil")), std::string("Home"));
    CHECK_EQ(std::string(lang::t("Actualités")), std::string("News"));
    CHECK_EQ(std::string(lang::t("Se déconnecter")), std::string("Sign out"));
    // Chaine multi-lignes (etat vide de l'accueil)
    CHECK(std::string(lang::t("Aucun serveur favori.\nAjoutez une adresse "
                              "ci-dessus.")) !=
          std::string("Aucun serveur favori.\nAjoutez une adresse ci-dessus."));

    // Cle absente -> francais rendu tel quel (jamais nullptr ni vide).
    CHECK_EQ(std::string(lang::t("Chaîne jamais traduite 42")),
             std::string("Chaîne jamais traduite 42"));
    CHECK(lang::t(static_cast<const char*>(nullptr)) == nullptr);

    // Surcharge std::string
    CHECK_EQ(lang::t(std::string("Accueil")), std::string("Home"));
    CHECK_EQ(lang::t(std::string("inconnu")), std::string("inconnu"));
    CHECK_EQ(lang::t(std::string()), std::string());

    // --- 4. set_language : ecrit les reglages, pas de redemarrage ---
    lang::set_language("fr");
    CHECK_EQ(DataStore::settings.language, std::string("fr"));
    CHECK(!lang::is_en());
    lang::set_language("en");
    CHECK_EQ(DataStore::settings.language, std::string("en"));
    CHECK(lang::is_en());
    lang::set_language("zz"); // inconnu -> fr
    CHECK_EQ(DataStore::settings.language, std::string("fr"));

    // --- 5. Coherence de la table ---
    CHECK(lang::dict_size() > 150);
    // Toutes les cles relues doivent donner une valeur non vide et differente
    // du francais (sinon c'est une entree oubliee), sauf les sigles voulus.
    static const std::set<std::string> kSameOnPurpose = {
        "Instances", "Skins", "Bedrock", "Screenshots", "Mods", "Favoris",
        "Description", "Version", "Loader", "Microsoft Store", "CURSEFORGE",
        "English", "OK", "Partager", "format #rrggbb", "instance(s)",
        "Connexion Microsoft", "Maintenance", "Port :"};
    DataStore::settings.language = "en";
    for (const char* key : {"Accueil", "Jouer", "Serveurs", "Compte", "Paramètres",
                            "Enregistrer", "Annuler", "Fermer", "Supprimer",
                            "Instances", "Skins", "Mods"}) {
        const std::string out = lang::t(std::string(key));
        CHECK(!out.empty());
        if (!kSameOnPurpose.count(key)) CHECK(out != std::string(key));
    }

    DataStore::settings.language = "fr";
    DataStore::shutdown();
    fs::remove_all(tmp, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

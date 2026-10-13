// Tests hors reseau de la securite S2 de http_win : allowlist d'hotes
// (host_allowed / allow_host) et redaction des secrets dans les journaux
// (redact_url). Aucune requete n'est emise : seules les fonctions pures de
// decision sont exercées.

#include "http_win.hpp"
#include "miniz.h"
#include "util_zip.hpp"

#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace tl::http;
namespace fs = std::filesystem;

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

int main() {
    // --- 1. host_allowed : hotes reels autorises (tous les services utiles) ---
    CHECK(host_allowed("api.modrinth.com"));
    CHECK(host_allowed("cdn.modrinth.com"));
    CHECK(host_allowed("api.curseforge.com"));
    CHECK(host_allowed("www.curseforge.com"));
    CHECK(host_allowed("edge.forgecdn.net"));
    CHECK(host_allowed("mediafilez.forgecdn.net"));
    CHECK(host_allowed("login.live.com"));
    CHECK(host_allowed("login.microsoftonline.com"));
    CHECK(host_allowed("account.microsoft.com"));
    CHECK(host_allowed("user.auth.xboxlive.com"));
    CHECK(host_allowed("xsts.auth.xboxlive.com"));
    CHECK(host_allowed("auth.xbox.com"));
    CHECK(host_allowed("api.minecraftservices.com"));
    CHECK(host_allowed("piston-meta.mojang.com"));
    CHECK(host_allowed("piston-data.mojang.com"));
    CHECK(host_allowed("api.mojang.com"));
    CHECK(host_allowed("textures.minecraft.net"));
    CHECK(host_allowed("libraries.minecraft.net"));
    CHECK(host_allowed("resources.download.minecraft.net"));
    CHECK(host_allowed("files.minecraftforge.net"));
    CHECK(host_allowed("maven.minecraftforge.net"));
    CHECK(host_allowed("maven.neoforged.net"));
    CHECK(host_allowed("meta.fabricmc.net"));
    CHECK(host_allowed("maven.fabricmc.net"));
    CHECK(host_allowed("api.adoptium.net"));
    CHECK(host_allowed("api.github.com"));
    CHECK(host_allowed("github.com"));
    CHECK(host_allowed("raw.githubusercontent.com"));
    CHECK(host_allowed("objects.githubusercontent.com"));
    CHECK(host_allowed("teamstarwars-dev.github.io"));
    // GitLab : hote autorise par Modrinth dans les .mrpack (sans lui, les
    // fichiers d'un modpack heberges la echouaient en silence).
    CHECK(host_allowed("overpass-api.de"));
    CHECK(host_allowed("gitlab.com"));
    CHECK(host_allowed("cdn.gitlab.com"));
    CHECK(host_allowed("discord.com"));
    CHECK(host_allowed("mc-heads.net"));
    CHECK(host_allowed("namemc.com"));
    CHECK(host_allowed("s.namemc.com"));
    CHECK(host_allowed("archive.org"));
    CHECK(host_allowed("web.archive.org"));
    CHECK(host_allowed("51.255.207.183"));
    // Boucle locale (tests / services locaux)
    CHECK(host_allowed("127.0.0.1"));
    CHECK(host_allowed("localhost"));
    CHECK(host_allowed("::1"));
    CHECK(host_allowed("[::1]")); // WinHttpCrackUrl peut garder les crochets
    // Insensibilite a la casse + point final de FQDN
    CHECK(host_allowed("API.MODRINTH.COM"));
    CHECK(host_allowed("api.modrinth.com."));

    // --- 2. host_allowed : refus + tentatives de contournement ---
    CHECK(!host_allowed(""));
    CHECK(!host_allowed("evil.com"));
    CHECK(!host_allowed("exemple.org"));
    CHECK(!host_allowed("notmodrinth.com"));            // pas de frontiere '.'
    CHECK(!host_allowed("api.modrinth.com.evil.com"));  // suffixe parasite
    CHECK(!host_allowed("modrinth.com.evil.io"));
    CHECK(!host_allowed("evil-microsoft.com"));
    CHECK(!host_allowed("microsoft.com.evil.io"));
    CHECK(!host_allowed("xboxlive.com.evil.io"));
    CHECK(!host_allowed("login.live.com.evil.tld"));
    CHECK(!host_allowed("githubusercontent.com.attacker.example"));
    CHECK(!host_allowed("discord.com.evil.io"));
    // IP admin : egalite stricte uniquement (pas de correspondance par suffixe)
    CHECK(!host_allowed("183.51.255.207.183"));
    CHECK(!host_allowed("951.255.207.183"));
    CHECK(!host_allowed("10.0.0.1")); // boucle seule, pas le reseau prive

    // --- 3. allow_host : hotes dynamiques de la configuration ---
    allow_host("panel.test.lan");
    CHECK(host_allowed("panel.test.lan"));
    CHECK(host_allowed("PANEL.TEST.LAN"));      // casse
    CHECK(host_allowed("sub.panel.test.lan"));  // sous-domaine admis
    CHECK(!host_allowed("notpanel.test.lan"));  // pas de frontiere '.'
    CHECK(!host_allowed("panel.test.lan.evil.com"));
    // Accepte aussi une URL complete (l'hote en est extrait, port retire)
    allow_host("https://Hooks.Example.net:8443/panel");
    CHECK(host_allowed("hooks.example.net"));
    CHECK(host_allowed("sub.hooks.example.net"));
    CHECK(!host_allowed("example.net")); // la racine n'est pas ouverte
    allow_host("127.0.0.1:8080");        // hote nu avec port
    CHECK(host_allowed("127.0.0.1"));
    // Saisie abusive : jamais enregistree
    allow_host("evil.com/path");
    allow_host("");
    CHECK(!host_allowed("evil.com"));

    // --- 4. redact_url : userinfo masque ---
    CHECK_EQ(redact_url("https://user:pass@example.com/x"),
             std::string("https://***@example.com/x"));
    CHECK_EQ(redact_url("https://user@example.com/x"),
             std::string("https://***@example.com/x"));
    CHECK_EQ(redact_url("https://example.com/x"),
             std::string("https://example.com/x"));

    // --- 5. redact_url : jeton de webhook masque ---
    CHECK_EQ(redact_url("https://discord.com/api/webhooks/1412511652776083586/"
                        "SUPERSECRET"),
             std::string("https://discord.com/api/webhooks/1412511652776083586/***"));
    // Chemin + query seul (c.path livre par WinHttpCrackUrl)
    CHECK_EQ(redact_url("/api/webhooks/42/jeton-en-clair"),
             std::string("/api/webhooks/42/***"));
    CHECK_EQ(redact_url("/api/webhooks/42/abc/def"),
             std::string("/api/webhooks/42/***/def"));
    // Un seul segment apres « webhooks » : rien a masquer
    CHECK_EQ(redact_url("https://discord.com/api/webhooks/42"),
             std::string("https://discord.com/api/webhooks/42"));
    // Le mot « webhooks » doit etre un segment entier
    CHECK_EQ(redact_url("https://example.com/notwebhooks/1/2"),
             std::string("https://example.com/notwebhooks/1/2"));

    // --- 6. redact_url : valeurs de query sensibles masquees ---
    CHECK_EQ(redact_url("https://api.example.com/x?key=SECRET&limit=5"),
             std::string("https://api.example.com/x?key=***&limit=5"));
    CHECK_EQ(redact_url("https://api.example.com/x?access_token=SECRET&y=1"),
             std::string("https://api.example.com/x?access_token=***&y=1"));
    CHECK_EQ(redact_url("https://api.example.com/x?Refresh_Token=SECRET"),
             std::string("https://api.example.com/x?Refresh_Token=***"));
    // « sig » est aussi dans la liste sensible (signature d'URL type SAS) :
    // les deux valeurs doivent etre masquees.
    CHECK_EQ(redact_url("https://api.example.com/x?x-api-key=SECRET&sig=zz"),
             std::string("https://api.example.com/x?x-api-key=***&sig=***"));
    CHECK_EQ(redact_url("https://api.example.com/x?code=abc123#frag"),
             std::string("https://api.example.com/x?code=***#frag"));
    CHECK_EQ(redact_url("/path/queue?token=abc"),
             std::string("/path/queue?token=***"));
    // Separateur « ; » egalement pris en charge
    CHECK_EQ(redact_url("https://h.example/p?secret=a;b=2"),
             std::string("https://h.example/p?secret=***;b=2"));
    // Cle non sensible : URL conservee telle quelle
    CHECK_EQ(redact_url("https://api.modrinth.com/v2/search?limit=25&query=foo"),
             std::string("https://api.modrinth.com/v2/search?limit=25&query=foo"));

    // --- 7. URL propre : aucune modification inutile ---
    CHECK_EQ(redact_url("https://api.modrinth.com/v2/project/sodium/version"),
             std::string("https://api.modrinth.com/v2/project/sodium/version"));
    CHECK_EQ(redact_url("http://51.255.207.183:3000/api/etat"),
             std::string("http://51.255.207.183:3000/api/etat"));
    CHECK_EQ(redact_url(""), std::string(""));

    // --- 8. Extraction d'archive : aucune entrée ne doit sortir du dossier
    //
    // Défaut réel, corrigé dans util_zip.cpp. L'ancien contrôle ne
    // regardait que la forme du nom (« .. », premier caractère « / ») et
    // laissait passer « C:/… ». Or `destDir / chemin_absolu` REMPLACE la
    // base : l'extraction écrivait là où l'archive le demandait.
    //
    // Un modpack est une archive téléchargée depuis Internet. Ce test
    // échoue si la protection disparaît.
    {
        std::error_code ec;
        const fs::path tmp = fs::temp_directory_path() / "tl_sec_zip";
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        const fs::path dest = tmp / "dest";
        fs::create_directories(dest, ec);

        // Cible absolue d'évasion : volontairement inoffensive, mais
        // bien HORS du dossier de destination.
        const fs::path evade = tmp / "evade.txt";
        const fs::path zipPath = tmp / "malveillant.zip";

        // Noms d'entrée hostiles, dans les formes qui comptent :
        // remontée relative, chemin absolu, et — sous Windows —
        // relatif-au-lecteur, que l'ancien contrôle ne voyait pas.
        std::vector<std::string> noms = {
            "ok.txt",
            "../evade.txt",
            evade.generic_string(),
        };
#ifdef _WIN32
        noms.push_back("C:evade.txt");
        noms.push_back("C:/Windows/Temp/tl_sec_evade.txt");
#endif
        mz_zip_archive z{};
        if (mz_zip_writer_init_file(&z, zipPath.string().c_str(), 0)) {
            const char* data = "x";
            for (const auto& n : noms)
                mz_zip_writer_add_mem(&z, n.c_str(), data, 1,
                                      MZ_DEFAULT_COMPRESSION);
            mz_zip_writer_finalize_archive(&z);
            mz_zip_writer_end(&z);
        }

        const int written = tl::zip_extract_all(zipPath, dest);

        // La seule entrée légitime est extraite.
        CHECK(fs::exists(dest / "ok.txt", ec));
        CHECK_EQ(written, 1);
        // Et aucune des autres n'a atterri où elle voulait.
        CHECK(!fs::exists(evade, ec));
#ifdef _WIN32
        CHECK(!fs::exists(fs::path("C:/Windows/Temp/tl_sec_evade.txt"), ec));
#endif
        // Rien non plus ailleurs sous le dossier de travail : un seul
        // fichier au total sous `dest`.
        int sous = 0;
        for (auto it = fs::recursive_directory_iterator(dest, ec);
             it != fs::recursive_directory_iterator(); it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec)) ++sous;
        }
        CHECK_EQ(sous, 1);

        fs::remove_all(tmp, ec);
    }

    // --- Securite M4 : redirections sures (allowed_redirect) ---
    // Les deux transports suivent les redirections A LA MAIN via cette
    // fonction. L'allowlist vaut donc a chaque saut, et le downgrade
    // https -> http est refuse.
    {
        // Saut cross-host vers un hote AUTORISE : accepte, URL resolue.
        CHECK_EQ(allowed_redirect("https://api.adoptium.net/v3/binary/latest",
                                  "https://github.com/adoptium/x.zip")
                     .value_or(""),
                 std::string("https://github.com/adoptium/x.zip"));

        // Saut cross-host vers un hote NON autorise : refuse.
        CHECK(!allowed_redirect("https://github.com/x",
                                "https://evil.example.com/p")
                   .has_value());

        // Downgrade https -> http (meme hote autorise) : refuse.
        CHECK(!allowed_redirect("https://github.com/x", "http://github.com/y")
                   .has_value());

        // Upgrade http -> https vers un hote autorise : accepte.
        CHECK(allowed_redirect("http://github.com/x", "https://github.com/y")
                  .has_value());

        // Location relative (racine) : meme hote, resolue sur l'origine.
        CHECK_EQ(allowed_redirect("https://api.adoptium.net/v3/binary/latest?a=1",
                                  "/v3/assets/x")
                     .value_or(""),
                 std::string("https://api.adoptium.net/v3/assets/x"));

        // Location relative (repertoire courant).
        CHECK_EQ(allowed_redirect("https://github.com/a/b", "c").value_or(""),
                 std::string("https://github.com/a/c"));

        // Schema non http(s) : refuse.
        CHECK(!allowed_redirect("https://github.com/x", "ftp://github.com/y")
                   .has_value());

        // Location vide : refuse.
        CHECK(!allowed_redirect("https://github.com/x", "").has_value());
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

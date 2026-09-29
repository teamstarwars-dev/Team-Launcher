// Tests du module 4c : BackupService, CrashAnalyzer, CleanupService,
// comparaison de versions et telemetrie (garde-fou « rien sans webhook »).

#include "backup.hpp"
#include "crash_analyzer.hpp"
#include "datastore.hpp"
#include "game_installer.hpp" // runtime_root
#include "maintenance.hpp"
#include "admin.hpp"
#include "presence.hpp"
#include "shortcut.hpp"
#include "telemetry.hpp"

#include "test_env.hpp" // _putenv_s portable (Windows/POSIX)
#include "util_zip.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

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
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void write_file(const fs::path& p, const std::string& s) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    return s;
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-services-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    _putenv_s("TL_RUNTIME_DIR", (tmp / "runtime").string().c_str());
    DataStore::load();

    // =====================================================================
    // 1. BackupService
    // =====================================================================
    const std::string inst = "instance-test";
    const fs::path saves = DataStore::instancesRoot() / inst / "saves";

    // Pas de dossier saves -> rien a sauvegarder
    CHECK_EQ(backup::create(inst), std::string(""));
    CHECK(backup::list(inst).empty());

    // Dossier saves vide -> rien non plus (fidele C# : EnumerateFileSystemEntries)
    fs::create_directories(saves, ec);
    CHECK_EQ(backup::create(inst), std::string(""));

    // Avec un monde : archive creee dans backups/
    write_file(saves / "monde1" / "level.dat", "DONNEES-MONDE-1");
    write_file(saves / "monde1" / "region" / "r.0.0.mca", "REGION");
    const std::string zip1 = backup::create(inst);
    CHECK(!zip1.empty());
    CHECK(fs::exists(zip1));
    CHECK_EQ(fs::path(zip1).parent_path(), backup::dir(inst));
    CHECK(fs::path(zip1).filename().string().rfind("mondes-", 0) == 0);
    {
        auto l = backup::list(inst);
        CHECK_EQ(l.size(), size_t{1});
    }

    // Restauration : le contenu revient, meme apres modification
    write_file(saves / "monde1" / "level.dat", "CORROMPU");
    write_file(saves / "monde2" / "level.dat", "AJOUTE-APRES");
    CHECK(backup::restore(inst, zip1));
    CHECK_EQ(read_file(saves / "monde1" / "level.dat"),
             std::string("DONNEES-MONDE-1"));
    CHECK(fs::exists(saves / "monde1" / "region" / "r.0.0.mca"));
    // monde2 n'etait pas dans l'archive : il disparait (remplacement complet)
    CHECK(!fs::exists(saves / "monde2"));
    // Aucun dossier temporaire « restore-* » ne subsiste
    for (const auto& e : fs::directory_iterator(DataStore::instancesRoot() / inst, ec))
        CHECK(e.path().filename().string().rfind("restore-", 0) != 0);

    // Archive illisible : le saves existant n'est PAS detruit (divergence C#)
    const fs::path bad = backup::dir(inst) / "corrompue.zip";
    write_file(bad, "ceci n'est pas un zip");
    CHECK(!backup::restore(inst, bad));
    CHECK_EQ(read_file(saves / "monde1" / "level.dat"),
             std::string("DONNEES-MONDE-1"));

    // Rotation : au-dela de kMaxBackups, les plus anciennes sont supprimees
    for (int i = 0; i < backup::kMaxBackups + 4; ++i) {
        char name[64];
        std::snprintf(name, sizeof(name), "mondes-2020-01-%02d_00-00.zip", i + 1);
        write_file(backup::dir(inst) / name, "faux zip");
    }
    fs::remove(bad, ec);
    write_file(saves / "monde1" / "level.dat", "DONNEES-MONDE-1");
    CHECK(!backup::create(inst).empty());
    CHECK(static_cast<int>(backup::list(inst).size()) <= backup::kMaxBackups);

    CHECK(backup::remove(backup::list(inst).front().file));
    CHECK(!backup::remove(backup::dir(inst) / "inexistante.zip"));

    // =====================================================================
    // 2. CrashAnalyzer
    // =====================================================================
    CHECK(!crash::analyze("tout va bien, le jeu se ferme normalement").has_value());
    CHECK(crash::analyze("java.lang.OutOfMemoryError: Java heap space").has_value());
    // Insensible a la casse (RegexOptions.IgnoreCase du C#)
    CHECK(crash::analyze("caused by: OUTOFMEMORYERROR").has_value());
    // Alternatives d'une meme regle
    CHECK(crash::analyze("java.lang.ClassNotFoundException: foo").has_value());
    CHECK(crash::analyze("NoClassDefFoundError: bar").has_value());
    CHECK_EQ(crash::analyze("ClassNotFoundException").value(),
             crash::analyze("NoClassDefFoundError").value());
    // Ordre des regles : la memoire passe avant les mods
    CHECK(crash::analyze("OutOfMemoryError et ModResolutionException").value() ==
          crash::analyze("OutOfMemoryError").value());
    // Chaque regle renvoie un conseil non vide avec une marche a suivre
    for (const char* k : {"UnsupportedClassVersionError", "Invalid session",
                          "Pixel format not accelerated",
                          "Could not reserve enough space for object heap",
                          "AccessDeniedException", "Connection timed out",
                          "DuplicateModsFoundException"}) {
        auto a = crash::analyze(std::string("...") + k + "...");
        CHECK(a.has_value());
        if (a) CHECK(a->find("→") != std::string::npos);
    }

    // tail_lines
    {
        const fs::path log = tmp / "tail.txt";
        std::string content;
        for (int i = 1; i <= 50; ++i) content += "ligne" + std::to_string(i) + "\n";
        write_file(log, content);
        const std::string t5 = crash::tail_lines(log, 5);
        CHECK(t5.rfind("ligne46", 0) == 0);
        CHECK(t5.find("ligne50") != std::string::npos);
        CHECK(t5.find("ligne45") == std::string::npos);
        // Moins de lignes que demande -> tout le fichier
        CHECK(crash::tail_lines(log, 999).find("ligne1\n") == 0);
        CHECK_EQ(crash::tail_lines(tmp / "absent.txt", 10), std::string(""));
    }

    // analyze_instance : crash-report recent prioritaire sur game-log
    {
        const fs::path dir = tmp / "gamedir";
        write_file(dir / "game-log.txt", "rien de special ici\n");
        CHECK(!crash::analyze_instance(dir).has_value());

        write_file(dir / "game-log.txt", "java.lang.OutOfMemoryError\n");
        auto fromLog = crash::analyze_instance(dir);
        CHECK(fromLog.has_value());

        write_file(dir / "crash-reports" / "crash-2026.txt",
                   "UnsupportedClassVersionError");
        auto fromReport = crash::analyze_instance(dir);
        CHECK(fromReport.has_value());
        CHECK(fromReport != fromLog); // le rapport l'emporte

        // Rapport non reconnu mais recent -> message generique
        write_file(dir / "crash-reports" / "crash-2026.txt", "panne inconnue");
        auto generic = crash::analyze_instance(dir);
        CHECK(generic.has_value());
        if (generic) CHECK(generic->find("crash-reports") != std::string::npos);

        // Dossier inexistant -> nullopt, pas d'exception
        CHECK(!crash::analyze_instance(tmp / "nexistepas").has_value());
    }

    // =====================================================================
    // 3. CleanupService
    // =====================================================================
    {
        const fs::path rt = runtime_root();
        write_file(rt / "forge-installer-1.12.2.jar", std::string(2048, 'x'));
        write_file(rt / "neoforge-installer-1.21.jar", std::string(1024, 'x'));
        write_file(rt / "adoptium-jre-17.zip", std::string(4096, 'x'));
        write_file(rt / "a-garder.jar", "important");       // nom non cible
        write_file(rt / "forge-installer.jar", "a garder"); // pas de suffixe

        const auto r = cleanup::run();
        CHECK_EQ(r.files, 3);
        CHECK(r.mb > 0.0);
        CHECK(!fs::exists(rt / "forge-installer-1.12.2.jar"));
        CHECK(!fs::exists(rt / "adoptium-jre-17.zip"));
        CHECK(fs::exists(rt / "a-garder.jar"));
        CHECK(fs::exists(rt / "forge-installer.jar"));

        // Deuxieme passage : plus rien a supprimer
        CHECK_EQ(cleanup::run().files, 0);

        // .zip recent du dossier de donnees : conserve (< 30 jours)
        write_file(DataStore::dir() / "recent.zip", "zip recent");
        CHECK_EQ(cleanup::run().files, 0);
        CHECK(fs::exists(DataStore::dir() / "recent.zip"));
    }

    // =====================================================================
    // 4. Comparaison de versions (UpdateService)
    // =====================================================================
    CHECK(updates::compare_versions("6.0.0", "6.0.0") == 0);
    CHECK(updates::compare_versions("6.0.1", "6.0.0") > 0);
    CHECK(updates::compare_versions("6.0.0", "6.0.1") < 0);
    CHECK(updates::compare_versions("6.1.0", "6.0.9") > 0);
    CHECK(updates::compare_versions("7.0", "6.9.9") > 0);
    // Longueurs differentes : les champs absents valent 0
    CHECK(updates::compare_versions("6.0", "6.0.0") == 0);
    CHECK(updates::compare_versions("6.0.0.1", "6.0.0") > 0);
    // Prefixe « v » et suffixes ignores
    CHECK(updates::compare_versions("v6.0.1", "6.0.0") > 0);
    CHECK(updates::compare_versions("6.0.0-beta", "6.0.0") == 0);
    CHECK(std::string(updates::current_version()).find("6.") == 0);

    // Cas limites de compare_versions
    CHECK(updates::compare_versions("", "") == 0);
    CHECK(updates::compare_versions("", "0") == 0);
    CHECK(updates::compare_versions("v", "6.0.0") < 0);
    CHECK(updates::compare_versions("06.0.0", "6.0.0") == 0); // zeros initiaux
    CHECK(updates::compare_versions("6.0.10", "6.0.9") > 0);  // numerique, pas lexical
    CHECK(updates::compare_versions("10.0", "9.9.9") > 0);
    CHECK(updates::compare_versions("6.0.0.0.1", "6.0.0") > 0);
    CHECK(updates::compare_versions("abc", "") == 0); // rien de numerique
    CHECK(updates::compare_versions("1.2a.3", "1.2.3") == 0);
    CHECK(updates::compare_versions("6..1", "6.0.1") == 0); // champ vide = 0
    CHECK(updates::compare_versions(" 6.0.1", "6.0.0") > 0); // espaces ignores

    // =====================================================================
    // 4bis. Selection d'asset (JSON GitHub factice, hors ligne ; preference
    // OS courant : Windows prefere win-x64, Linux prefere linux-x64)
    // =====================================================================
    {
        const nlohmann::json rel = {
            {"tag_name", "v9.9.9"},
            {"assets",
             {{{"name", "team-launcher-linux-x64.tar.gz"},
               {"browser_download_url", "https://example.invalid/linux"},
               {"size", 10}},
              {{"name", "team-launcher-win32.zip"},
               {"browser_download_url", "https://example.invalid/win32"},
               {"size", 20}},
              {{"name", "Team-Launcher-Win-x64.zip"},
               {"browser_download_url", "https://example.invalid/winx64"},
               {"size", 30}},
              {{"name", "Team-Launcher-Linux-x64.zip"},
               {"browser_download_url", "https://example.invalid/linuxzip"},
               {"size", 25}},
              {{"name", "setup.exe"},
               {"browser_download_url", "https://example.invalid/setup"},
               {"size", 40}}}}};
        std::string name;
        long long size = -1;
#ifdef _WIN32
        CHECK_EQ(updates::select_asset_url(rel, &name, &size),
                 std::string("https://example.invalid/winx64"));
        CHECK_EQ(name, std::string("Team-Launcher-Win-x64.zip"));
        CHECK_EQ(size, 30LL);
#else
        CHECK_EQ(updates::select_asset_url(rel, &name, &size),
                 std::string("https://example.invalid/linuxzip"));
        CHECK_EQ(name, std::string("Team-Launcher-Linux-x64.zip"));
        CHECK_EQ(size, 25LL);
#endif

        // Sans zip Windows : chaine vide sous Windows (l'UI proposera la page
        // web) ; sous Linux le tar.gz n'est pas un .zip exploitable non plus.
        const nlohmann::json linuxOnly = {
            {"tag_name", "v9.9.9"},
            {"assets",
             {{{"name", "app-linux.tar.gz"},
               {"browser_download_url", "https://example.invalid/l"},
               {"size", 5}}}}};
        CHECK_EQ(updates::select_asset_url(linuxOnly), std::string(""));
        // Pas de tableau assets / asset sans URL : ignore proprement
        CHECK_EQ(updates::select_asset_url(nlohmann::json::object()),
                 std::string(""));
        const nlohmann::json noUrl = {
            {"assets", {{{"name", "team-win-x64.zip"}, {"size", 7}}}}};
        CHECK_EQ(updates::select_asset_url(noUrl), std::string(""));
        CHECK_EQ(updates::select_asset_url(nlohmann::json::array()),
                 std::string(""));
    }

    // =====================================================================
    // 4ter. parse_release_json (hors ligne ; asset de l'OS courant)
    // =====================================================================
    {
        // Plus recent que la version compilee : info complete + asset
#ifdef _WIN32
        const std::string assetName = "team-launcher-win-x64.zip";
#else
        const std::string assetName = "team-launcher-linux-x64.zip";
#endif
        const std::string body =
            R"({"tag_name":"v9.9.9","body":"notes","html_url":"https://example.invalid/r",)"
            R"("assets":[{"name":")" +
            assetName +
            R"(","browser_download_url":"https://example.invalid/z","size":123}]})";
        std::string err;
        auto info = updates::parse_release_json(body, &err);
        CHECK(info.has_value());
        CHECK(err.empty());
        if (info) {
            CHECK_EQ(info->version, std::string("9.9.9"));
            CHECK_EQ(info->notes, std::string("notes"));
            CHECK_EQ(info->assetUrl, std::string("https://example.invalid/z"));
            CHECK_EQ(info->assetSize, 123LL);
        }
        // Pas plus recent : nullopt SANS erreur (deja a jour)
        err = "sentinelle";
        CHECK(!updates::parse_release_json(R"({"tag_name":"v0.0.1"})", &err));
        CHECK_EQ(err, std::string("sentinelle"));
        // Tag manquant / JSON invalide : nullopt AVEC erreur
        CHECK(!updates::parse_release_json(R"({"body":"x"})", &err));
        CHECK(!err.empty());
        CHECK(!updates::parse_release_json("ceci n'est pas du json", &err));
        CHECK(!err.empty());
    }

    // =====================================================================
    // 4quater. Marqueur staged (hors ligne, dossier de test uniquement)
    // =====================================================================
    {
        CHECK(!updates::has_staged()); // dossier de test frais
        CHECK(!updates::staged().has_value());

        // Marqueur ecrit a la main + zip factice : staged() le retrouve
        const fs::path updDir = updates::updates_dir();
        write_file(updDir / "team-launcher-9.9.9.zip", "faux zip");
        const nlohmann::json marker = {
            {"version", "9.9.9"}, {"notes", "n"}, {"url", "u"},
            {"assetUrl", "a"},    {"assetName", "team-launcher-9.9.9.zip"},
            {"assetSize", 8LL},   {"file", (updDir / "team-launcher-9.9.9.zip").string()}};
        write_file(updDir / "pending.json", marker.dump(2));
        CHECK(updates::has_staged());
        auto st = updates::staged();
        CHECK(st.has_value());
        if (st) {
            CHECK_EQ(st->info.version, std::string("9.9.9"));
            CHECK(fs::exists(st->file));
        }
        // Zip manquant -> pas de staged (marqueur orphelin ignore)
        fs::remove(updDir / "team-launcher-9.9.9.zip", ec);
        CHECK(!updates::has_staged());
        // clear_staged() nettoie le marqueur orphelin
        CHECK(updates::clear_staged());
        CHECK(!fs::exists(updDir / "pending.json"));
        CHECK(!updates::has_staged());
    }

    // =====================================================================
    // 5. Telemetrie : rien ne part sans webhook
    // =====================================================================
    {
        nlohmann::json fake = {{"Id", "x"}, {"Name", "Test"}, {"Loader", "Forge"},
                               {"McVersion", "1.12.2"}, {"Launches", 3}};
        DataStore::settings.telemetryEnabled = true;
        DataStore::settings.discordTelemetryWebhook = "";
        CHECK(!telemetry::enabled());
        telemetry::report_crash(fake, -1, "log");
        telemetry::report_launch(fake);
        telemetry::report_startup();

        // Active + webhook -> enabled, mais on n'envoie rien vers un vrai serveur
        DataStore::settings.discordTelemetryWebhook = "https://example.invalid/hook";
        CHECK(telemetry::enabled());
        DataStore::settings.telemetryEnabled = false;
        CHECK(!telemetry::enabled());
        DataStore::settings.discordTelemetryWebhook = "";

        telemetry::stop(); // aucun worker lance : sans effet
    }

    // =====================================================================
    // 5bis. Raccourci (.lnk Windows / .desktop Linux, AutoShortcut)
    // =====================================================================
    {
        // On ecrit dans le dossier de test, jamais sur le vrai Bureau.
#ifdef _WIN32
        const fs::path lnk = tmp / "raccourci" / "Team Launcher.lnk";
#else
        const fs::path lnk = tmp / "raccourci" / "Team Launcher.desktop";
#endif
        const fs::path target = tmp / "faux-launcher.exe";
        write_file(target, "MZ");

        CHECK(create_shortcut(lnk, target, target.parent_path(), "Test"));
        CHECK(fs::exists(lnk));
        CHECK(fs::file_size(lnk, ec) > 0);
#ifdef _WIN32
        // Un .lnk commence par l'en-tete ShellLink : 4C 00 00 00 ("L").
        {
            const std::string raw = read_file(lnk);
            CHECK(raw.size() > 4);
            if (raw.size() > 4) {
                CHECK_EQ(static_cast<unsigned char>(raw[0]), 0x4Cu);
                CHECK_EQ(static_cast<unsigned char>(raw[1]), 0x00u);
            }
        }
#else
        // Un .desktop contient son type + la cible.
        {
            const std::string raw = read_file(lnk);
            CHECK(raw.find("[Desktop Entry]") != std::string::npos);
            CHECK(raw.find("Type=Application") != std::string::npos);
            CHECK(raw.find("Exec=") != std::string::npos);
            CHECK(raw.find(target.string()) != std::string::npos);
        }
#endif
        // Cible vide : refus propre, aucun fichier cree
        const fs::path none = tmp / "raccourci" / "vide.lnk";
        CHECK(!create_shortcut(none, {}, {}, "x"));
        CHECK(!fs::exists(none));

#ifdef _WIN32
        // desktop_dir() doit rendre un dossier existant sur une session Windows
        const fs::path desk = desktop_dir();
        CHECK(!desk.empty());
        if (!desk.empty()) CHECK(fs::is_directory(desk, ec));
#else
        // HOME isolé + Bureau créé : déterministe même sans session graphique.
        setenv("HOME", tmp.string().c_str(), 1);
        fs::create_directories(tmp / "Desktop", ec);
        const fs::path desk = desktop_dir();
        CHECK(desk == tmp / "Desktop");
#endif

        // ensure_desktop_shortcut(force=false) ne doit RIEN faire quand le
        // drapeau est deja pose : c'est ce qui evite de recreer le raccourci
        // a chaque demarrage si l'utilisateur l'a supprime.
        DataStore::settings.autoShortcut = true;
        CHECK(!ensure_desktop_shortcut(/*force=*/false));
    }

    // =====================================================================
    // 5ter. Rich Presence Discord : garde-fous (aucune IPC declenchee)
    // =====================================================================
    {
        const bool savedEn = DataStore::settings.discordEnabled;
        const std::string savedId = DataStore::settings.discordAppId;

        // Desactivee : enabled() faux, init() sans effet, shutdown() sans thread
        DataStore::settings.discordEnabled = false;
        DataStore::settings.discordAppId = "123";
        CHECK(!presence::enabled());
        presence::init();
        CHECK(!presence::connected());
        presence::set_launcher(); // ne doit rien demarrer
        CHECK(!presence::connected());
        presence::shutdown();

        // Activee mais sans identifiant : toujours desactivee (regle du C#)
        DataStore::settings.discordEnabled = true;
        DataStore::settings.discordAppId = "";
        CHECK(!presence::enabled());
        presence::init();
        CHECK(!presence::connected());
        presence::shutdown();

        // Les deux renseignes : enabled() vrai (on ne lance pas l'IPC ici)
        DataStore::settings.discordAppId = "123456789";
        CHECK(presence::enabled());

        // reload() alors que c'est desactive ne doit laisser aucun thread
        DataStore::settings.discordEnabled = false;
        presence::reload();
        CHECK(!presence::connected());

        DataStore::settings.discordEnabled = savedEn;
        DataStore::settings.discordAppId = savedId;
    }

    // =====================================================================
    // 5quater. Telemetrie d'administration (AdminService)
    // =====================================================================
    {
        const bool savedEn = DataStore::settings.adminTelemetryEnabled;
        const std::string savedUrl = DataStore::settings.adminServerUrl;
        const std::string savedId = DataStore::settings.installationId;

        // L'URL de test pointe vers un hote injoignable : meme si un envoi
        // partait par erreur, il n'atteindrait aucun vrai serveur.
        DataStore::settings.adminServerUrl = "http://127.0.0.1:9/";

        // Desactivee : rien ne part, start() ne lance aucun thread.
        DataStore::settings.adminTelemetryEnabled = false;
        CHECK(!admin::enabled());
        admin::start();
        admin::send_heartbeat();
        admin::send_event("test");
        admin::report_error("src", "msg");
        admin::stop(); // sans thread : sans effet

        // Activee mais sans URL : toujours desactivee
        DataStore::settings.adminTelemetryEnabled = true;
        DataStore::settings.adminServerUrl = "";
        CHECK(!admin::enabled());
        DataStore::settings.adminServerUrl = "http://127.0.0.1:9/";
        CHECK(admin::enabled());

        // Identifiant d'installation : genere une fois, puis stable
        DataStore::settings.installationId.clear();
        const std::string id1 = admin::installation_id();
        CHECK_EQ(id1.size(), size_t{32}); // Guid .NET « N »
        CHECK_EQ(admin::installation_id(), id1); // pas regenere
        CHECK_EQ(DataStore::settings.installationId, id1); // conserve

        // Informations machine : lues par API native, sans sous-processus.
        const auto m = admin::machine_info();
        CHECK(!m.hostname.empty());
        CHECK(m.ramMb > 0);
        CHECK(!m.osVersion.empty());
        std::printf("INFO machine : %s | %s | RAM %lld Mo\n", m.osVersion.c_str(),
                    m.cpuName.c_str(), m.ramMb);
        // Le C# renvoyait des champs vides sur Windows 11 24H2 (wmic absent) :
        // ici CPU et GPU doivent etre reellement remplis.
        CHECK(!m.cpuName.empty());
        CHECK(!m.gpuName.empty());

        // Forme du battement : memes cles que le C#, le serveur ne change pas.
        const auto p = admin::heartbeat_payload();
        for (const char* k : {"instance_id", "hostname", "os_version",
                              "launcher_version", "ram_mb", "cpu_name",
                              "gpu_name", "mc_version", "instance_count"})
            CHECK(p.contains(k));
        CHECK_EQ(p.value("instance_id", ""), id1);

        DataStore::settings.adminTelemetryEnabled = savedEn;
        DataStore::settings.adminServerUrl = savedUrl;
        DataStore::settings.installationId = savedId;
    }

    // =====================================================================
    // 6. Diagnostic (reseau : TL_TEST_NET=1)
    // =====================================================================
    if (std::getenv("TL_TEST_NET")) {
        const auto checks = health::run_all();
        CHECK_EQ(checks.size(), size_t{5});
        for (const auto& c : checks) {
            CHECK(!c.name.empty());
            CHECK(!c.detail.empty());
            std::printf("INFO diag %-34s %s — %s\n", c.name.c_str(),
                        c.ok ? "OK" : "KO", c.detail.c_str());
        }
        // Le dossier des instances doit etre accessible dans le dossier de test
        CHECK(checks[3].ok);
    } else {
        std::printf("INFO diagnostic reseau saute (TL_TEST_NET non defini)\n");
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

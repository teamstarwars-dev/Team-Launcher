#include "game_installer.hpp"
#include "game_launcher.hpp"
#include "http_win.hpp"
#include "util_hash.hpp"
#include "util_parallel.hpp"
#include "util_zip.hpp"

#include "test_env.hpp" // _putenv_s portable (Windows/POSIX)

#include "miniz.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

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
    const fs::path tmp = fs::temp_directory_path() / "tl-game-port-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
    _putenv_s("TL_RUNTIME_DIR", (tmp / "runtime").string().c_str());
#else
    setenv("TL_DATA_DIR", tmp.string().c_str(), 1);
    setenv("TL_RUNTIME_DIR", (tmp / "runtime").string().c_str(), 1);
#endif

    using namespace tl;

    // --- 1. MavenNameToPath ---
    CHECK_EQ(maven_name_to_path("com.mojang:brigadier:1.0.18"),
             (fs::path("com") / "mojang" / "brigadier" / "1.0.18" /
              "brigadier-1.0.18.jar").string());
    CHECK_EQ(maven_name_to_path("org.lwjgl:lwjgl:3.3.1:natives-windows"),
             (fs::path("org") / "lwjgl" / "lwjgl" / "3.3.1" /
              "lwjgl-3.3.1-natives-windows.jar").string());
    CHECK(maven_name_to_path("net.minecraftforge:mcp_config:1.16.5@zip").empty());
    CHECK(maven_name_to_path("trop-court").empty());

    // --- 2. RulesAllow (OS courant : "windows" sous Windows, "linux" sinon) ---
    CHECK(rules_allow(nlohmann::json::object())); // pas de rules -> true
#ifdef _WIN32
    constexpr const char* kOs = "windows";
    constexpr const char* kOther = "linux";
#else
    constexpr const char* kOs = "linux";
    constexpr const char* kOther = "windows";
#endif
    {
        const nlohmann::json osAllow = {{"rules", nlohmann::json::array({
            {{"action", "allow"}, {"os", {{"name", kOs}}}}})}};
        CHECK(rules_allow(osAllow));
        const nlohmann::json otherOnly = {{"rules", nlohmann::json::array({
            {{"action", "allow"}, {"os", {{"name", kOther}}}}})}};
        CHECK(!rules_allow(otherOnly));
        const nlohmann::json allowThenDenyOs = {{"rules", nlohmann::json::array({
            {{"action", "allow"}},
            {{"action", "disallow"}, {"os", {{"name", kOs}}}}})}};
        CHECK(!rules_allow(allowThenDenyOs));
        const nlohmann::json allowThenDenyOther = {{"rules", nlohmann::json::array({
            {{"action", "allow"}},
            {{"action", "disallow"}, {"os", {{"name", kOther}}}}})}};
        CHECK(rules_allow(allowThenDenyOther));
        const nlohmann::json noOs = {{"rules", nlohmann::json::array({
            {{"action", "allow"}}})}};
        CHECK(rules_allow(noOs));
    }

    // --- 3. ExtractJvmArgs ---
    {
        const nlohmann::json root = {
            {"arguments", {{"jvm", nlohmann::json::array({
                "-cp", "${classpath}",
                nlohmann::json{{"rules", nlohmann::json::array(
                    {{{"action", "allow"}, {"os", {{"name", kOs}}}}})},
                    {"value", "-Djava.library.path=${natives_directory}"}},
                nlohmann::json{{"rules", nlohmann::json::array(
                    {{{"action", "allow"}, {"os", {{"name", "osx"}}}}})},
                    {"value", "-XstartOnFirstThread"}},
            })}}}};
            const auto jvm = extract_jvm_args(root);
            CHECK_EQ(jvm.size(), size_t{3});
            CHECK_EQ(jvm[0], std::string("-cp"));
            CHECK_EQ(jvm[2], std::string("-Djava.library.path=${natives_directory}"));
        }
        CHECK(extract_jvm_args(nlohmann::json::object()).empty());

        // --- 4. OfflineSession (reference .NET Guid) ---
        {
            const McSession s = offline_session("Steve");
            CHECK_EQ(s.name, std::string("Steve"));
            CHECK_EQ(s.accessToken, std::string("0"));
            CHECK_EQ(s.uuid, std::string("98dd2756bee621bcf8a8e92344183641"));
        }

        // --- 5. SHA1 et MD5 : vecteurs officiels ---
        // Etape 5 : ces deux fonctions ne passent plus par BCrypt mais par une
        // implementation portable. Les vecteurs des RFC verrouillent le
        // resultat, y compris les cas de bourrage delicats (55, 56, 64
        // octets, et un flux de plusieurs blocs).
        {
            auto sha1_of = [&](const std::string& data, const char* tag) {
                const fs::path f = tmp / (std::string("sha1-") + tag + ".bin");
                { std::ofstream o(f, std::ios::binary); o << data; }
                return sha1_hex(f);
            };
            auto expect = [&](const std::string& data, const char* tag,
                              const char* hex) {
                const auto h = sha1_of(data, tag);
                CHECK(h.has_value());
                if (h) CHECK_EQ(*h, std::string(hex));
            };

            expect("", "vide", "da39a3ee5e6b4b0d3255bfef95601890afd80709");
            expect("abc", "abc", "a9993e364706816aba3e25717850c26c9cd0d89d");
            expect("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                   "rfc2", "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
            // 55 octets : dernier cas tenant dans un bloc avec le bourrage.
            expect(std::string(55, 'a'), "a55",
                   "c1c8bbdc22796e28c0e15163d20899b65621d65a");
            // 56 octets : force un bloc de bourrage supplementaire.
            expect(std::string(56, 'a'), "a56",
                   "c2db330f6083854c99d4b5bfb6e8f29f201be699");
            expect(std::string(64, 'a'), "a64",
                   "0098ba824b5c16427bd7a1122a5a442a25ec644d");
            // 1 million de 'a' : le vecteur long de la RFC 3174, qui exerce le
            // decoupage en blocs et le compteur de longueur sur 64 bits.
            expect(std::string(1000000, 'a'), "a1m",
                   "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

            CHECK(!sha1_hex(tmp / "absent.txt").has_value());

            auto md5_hex = [](const std::string& s) {
                const auto d = md5_digest(s);
                std::string out;
                if (!d) return out;
                static const char* kHex = "0123456789abcdef";
                for (unsigned char c : *d) {
                    out.push_back(kHex[c >> 4]);
                    out.push_back(kHex[c & 0x0F]);
                }
                return out;
            };
            CHECK_EQ(md5_hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
            CHECK_EQ(md5_hex("a"), std::string("0cc175b9c0f1b6a831c399e269772661"));
            CHECK_EQ(md5_hex("abc"), std::string("900150983cd24fb0d6963f7d28e17f72"));
            CHECK_EQ(md5_hex("message digest"),
                     std::string("f96b697d7cb7938d525a2f31aaf161d0"));
            CHECK_EQ(md5_hex("abcdefghijklmnopqrstuvwxyz"),
                     std::string("c3fcd3d76192e4007dfb496cca67e13b"));
            CHECK_EQ(md5_hex("123456789012345678901234567890123456789012345678"
                             "90123456789012345678901234567890"),
                     std::string("57edf4a22be3c955ac49da2e2107b67a"));
            CHECK_EQ(md5_hex(std::string(56, 'a')),
                     std::string("3b0c8ac703f828b04c6c197006d17218"));
        }

        // --- 5bis. RAM idéale (règle déjà spécifiée dans l'en-tête) ---
        // Moitié de la RAM physique, bornée [2, 8] Go ; 4 Go si la machine
        // est indéterminée. La fonction etait DECLAREE mais jamais definie
        // ni appelee : ces vecteurs verrouillent la regle.
        {
            CHECK_EQ(ideal_ram_gb(0), 4);      // indetermine
            CHECK_EQ(ideal_ram_gb(-1), 4);
            CHECK_EQ(ideal_ram_gb(2048), 2);   // 2 Go -> plancher
            CHECK_EQ(ideal_ram_gb(4096), 2);   // moitie = 2
            CHECK_EQ(ideal_ram_gb(8192), 4);   // 8 Go -> 4
            CHECK_EQ(ideal_ram_gb(16384), 8);  // 16 Go -> 8
            CHECK_EQ(ideal_ram_gb(32768), 8);  // 32 Go -> plafond
            CHECK_EQ(ideal_ram_gb(131072), 8); // 128 Go -> plafond
            CHECK_EQ(ideal_ram_gb(12288), 6);  // 12 Go -> 6
            // Machine tres modeste : jamais en dessous de 2 Go, sinon le jeu
            // ne demarre pas.
            CHECK_EQ(ideal_ram_gb(1024), 2);
            CHECK_EQ(ideal_ram_gb(1), 2);
            // La version sans argument doit rester dans les memes bornes.
            const int live = ideal_ram_gb();
            CHECK(live >= 2 && live <= 8);
            std::printf("INFO ram ideale sur cette machine : %d Go\n", live);
        }

        // --- 6. BuildJvmArgs ---
        {
            const auto args = build_jvm_args("CP", "NAT", false, nullptr, 6,
                                             "  -XX:+UseG1GC  -Xss1m ");
            CHECK_EQ(args[0], std::string("-Xmx6G"));
            CHECK_EQ(args[1], std::string("-Djava.library.path=NAT"));
            CHECK_EQ(args[2], std::string("-cp"));
            CHECK_EQ(args[3], std::string("CP"));
            CHECK_EQ(args[4], std::string("-XX:+UseG1GC"));
            CHECK_EQ(args[5], std::string("-Xss1m"));
            CHECK(std::find(args.begin(), args.end(),
                            "-Dsun.java2d.d3d=false") == args.end());

            const std::vector<std::string> official = {
                "-Djava.library.path=${natives_directory}",
                "-cp", "${classpath}",
                "-Dfoo=${classpath_separator}",
                "", // ignore
            };
            const auto off = build_jvm_args("CP", "NAT", false, &official, 4, "");
            CHECK_EQ(off[1], std::string("-Djava.library.path=NAT"));
            CHECK_EQ(off[3], std::string("CP"));
#ifdef _WIN32
            CHECK_EQ(off[4], std::string("-Dfoo=;")); // classpath_separator Windows
#else
            CHECK_EQ(off[4], std::string("-Dfoo=:")); // classpath_separator POSIX
#endif
            CHECK_EQ(off.size(), size_t{5}); // "" ignore

            const auto forge = build_jvm_args("CP", "NAT", true, nullptr, 4, "");
            CHECK(std::find(forge.begin(), forge.end(),
                            "-Dsun.java2d.d3d=false") != forge.end());
            CHECK(std::find(forge.begin(), forge.end(),
                            "-XX:ParallelGCThreads=2") != forge.end());
            CHECK(std::find(forge.begin(), forge.end(),
                            "-Dfml.ignorePatchDiscrepancies=true") != forge.end());
            const auto forge8 = build_jvm_args("CP", "NAT", true, nullptr, 8, "");
            CHECK(std::find(forge8.begin(), forge8.end(),
                            "-XX:ParallelGCThreads=2") == forge8.end());
        }

        // --- 7. BuildGameArgs moderne + joinServer ---
        {
            const McSession s = offline_session("Steve");
            const auto a = build_game_args("1.20.1", s, "17", nullptr, false,
                                           nullptr);
            CHECK_EQ(a[0], std::string("--username"));
            CHECK_EQ(a[1], std::string("Steve"));
            CHECK_EQ(a[9], std::string("17"));
            CHECK_EQ(a[11], std::string("98dd2756bee621bcf8a8e92344183641"));

            const std::string join = "mc.hypixel.net:25565";
            const auto b = build_game_args("1.20.1", s, "17", nullptr, false, &join);
            CHECK_EQ(b[b.size() - 4], std::string("--server"));
            CHECK_EQ(b[b.size() - 3], std::string("mc.hypixel.net"));
            CHECK_EQ(b[b.size() - 2], std::string("--port"));
            CHECK_EQ(b[b.size() - 1], std::string("25565"));

            const std::string joinNoPort = "serveur.local";
            const auto c = build_game_args("1.20.1", s, "17", nullptr, false,
                                           &joinNoPort);
            CHECK_EQ(c[c.size() - 2], std::string("--server"));
            CHECK_EQ(c[c.size() - 1], std::string("serveur.local"));
        }

        // --- 8. BuildGameArgs legacy ---
        {
            const McSession s = offline_session("Steve");
            const std::string legacy =
                "--username ${auth_player_name} --version ${version_name} "
                "--assetIndex ${assets_index_name} --userProperties ${user_properties}";
            const auto a = build_game_args("1.5.2", s, "legacy", &legacy, false,
                                           nullptr);
            CHECK_EQ(a[1], std::string("Steve"));
            CHECK_EQ(a[3], std::string("1.5.2"));
            CHECK_EQ(a[5], std::string("legacy"));
            CHECK_EQ(a[7], std::string("{}"));
        }

        // --- 9. parallel_for : compte + propagation d'exception ---
        {
            std::atomic<int> count{0};
            parallel_for(100, 8, [&](int) { count.fetch_add(1); });
            CHECK_EQ(count.load(), 100);

            bool threw = false;
            try {
                parallel_for(100, 8, [&](int i) {
                    if (i == 42) throw std::runtime_error("boom");
                });
            } catch (const std::runtime_error&) {
                threw = true;
            }
            CHECK(threw);
        }

        // --- 10. zip : extraction natives + garde-fou zip-slip ---
        {
            const fs::path z = tmp / "t.zip";
#ifdef _WIN32
            constexpr const char* kNat = "sub/a.dll";
            constexpr const char* kEvil = "../evil.dll";
            constexpr const char* kEvilBase = "evil.dll";
#else
            constexpr const char* kNat = "sub/a.so";
            constexpr const char* kEvil = "../evil.so";
            constexpr const char* kEvilBase = "evil.so";
#endif
            mz_zip_archive arch{};
            CHECK(mz_zip_writer_init_file(&arch, z.string().c_str(), 0));
            CHECK(mz_zip_writer_add_mem(&arch, kNat, "AAA", 3,
                                        MZ_DEFAULT_COMPRESSION));
            CHECK(mz_zip_writer_add_mem(&arch, kEvil, "BBB", 3,
                                        MZ_DEFAULT_COMPRESSION));
            CHECK(mz_zip_writer_add_mem(&arch, "install_profile.json", "{}", 2,
                                        MZ_DEFAULT_COMPRESSION));
            CHECK(mz_zip_writer_finalize_archive(&arch));
            mz_zip_writer_end(&arch);

            const fs::path outDir = tmp / "zipout";
            const int n = zip_extract_natives(z, outDir);
            CHECK_EQ(n, 1);
            CHECK(fs::exists(outDir / kNat));
            CHECK(!fs::exists(tmp / kEvilBase)); // traversal rejete
            CHECK(!fs::exists(outDir / ".." / kEvilBase));

            const fs::path prof = tmp / "extracted-profile.json";
            CHECK(zip_extract_entry(z, "install_profile.json", prof));
            CHECK(fs::exists(prof));
            CHECK(!zip_extract_entry(z, "absent.json", prof));
        }

        // --- 11. RAM + FindJava (machine reelle, sans assert dur) ---
        {
            const long long total = total_ram_mb();
            const long long avail = available_ram_mb();
            std::printf("INFO ram total=%lld avail=%lld\n", total, avail);
            CHECK(total == -1 || total > 0);

            const auto java = find_java(8);
            if (java) {
                const int major = detect_java_major(*java);
                std::printf("INFO find_java -> %s (major %d)\n", java->c_str(), major);
                CHECK(major >= 8);
            } else {
                std::printf("INFO find_java: aucun Java 8+ trouve (normal si absent)\n");
            }
        }

        // --- 12. Reseau (gated TL_TEST_NET=1) : manifeste Mojang + WinHTTP ---
        if (std::getenv("TL_TEST_NET")) {
            const auto body = http::get_string(
                "https://piston-meta.mojang.com/mc/game/version_manifest_v2.json");
            CHECK(body.has_value());
            if (body) {
                CHECK(body->find("\"latest\"") != std::string::npos);
                const auto doc = nlohmann::json::parse(*body);
                CHECK(doc.contains("latest"));
            }
            // redirection (Adoptium -> GitHub) : entete only, limite 3s
            const bool redirected = http::get_string(
                "https://api.adoptium.net/v3/info/available_releases")
                .has_value();
            CHECK(redirected);

            const auto rel = latest_release();
            std::printf("INFO latest release: %s\n",
                        rel ? rel->c_str() : "(null)");
            CHECK(rel.has_value());
        }

        // --- 13. Installeur : parsing sans reseau (echec attendu propre) ---
        if (std::getenv("TL_TEST_NET")) {
            std::atomic<bool> cancel{false};
            // ForceVerify + version inexistante -> exception propre, pas de crash
            bool threw = false;
            try {
                install("version-qui-nexiste-pas-9.9.9", "Vanilla",
                        nullptr, cancel, false);
            } catch (const std::exception& ex) {
                threw = true;
                std::printf("INFO install echec propre: %s\n", ex.what());
            }
            CHECK(threw);
        }

        // --- 14. zip roundtrip : export d'instance (zip_create_from_dir) ---
        {
            const fs::path src = tmp / "zip-src";
            fs::create_directories(src / "sub");
            std::ofstream(src / "a.txt") << "hello";
            std::ofstream(src / "sub" / "b.txt") << "world";
            const fs::path z = tmp / "export.zip";
            CHECK(zip_create_from_dir(src, z));
            CHECK(fs::is_regular_file(z));
            const fs::path out = tmp / "zip-out";
            CHECK_EQ(zip_extract_all(z, out), 2);
            std::string a, b;
            {
                std::ifstream ia(out / "a.txt");
                std::getline(ia, a);
            }
            {
                std::ifstream ib(out / "sub" / "b.txt");
                std::getline(ib, b);
            }
            CHECK_EQ(a, std::string("hello"));
            CHECK_EQ(b, std::string("world"));
            // dossier inexistant -> echec propre
            CHECK(!zip_create_from_dir(tmp / "absent-zip", tmp / "no.zip"));
        }

        fs::remove_all(tmp, ec);

        if (g_failures) {
            std::printf("TESTS FAILED: %d\n", g_failures);
            return 1;
        }
        std::printf("ALL TESTS PASSED\n");
        return 0;
}

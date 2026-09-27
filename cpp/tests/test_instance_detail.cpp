// Tests offline des helpers purs du portage InstanceDetailWindow.cs :
// format de taille FR, etat/tri des mods, resolution du fichier de log.
// La modale ImGui elle-meme n'est pas testable sans UI.

#define TL_DETAIL_LOGIC_ONLY
#include "../src/ui_instancedetail.cpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tl::ui::detail;

static int g_failures = 0;

#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

#define CHECK_EQ(a, b)                                                   \
    do {                                                                 \
        const auto va = (a);                                             \
        const auto vb = (b);                                             \
        if (!(va == vb)) {                                               \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                \
        }                                                                \
    } while (0)

int main() {
    // --- 1. format_size_fr (C# $"{x:0.#} Mo/Ko", virgule FR) ---
    CHECK_EQ(format_size_fr(0), std::string("0 Ko"));
    CHECK_EQ(format_size_fr(512), std::string("0,5 Ko"));
    CHECK_EQ(format_size_fr(1024), std::string("1 Ko"));
    CHECK_EQ(format_size_fr(1536), std::string("1,5 Ko"));
    CHECK_EQ(format_size_fr(2048), std::string("2 Ko"));
    CHECK_EQ(format_size_fr(1048576), std::string("1024 Ko")); // C# : mib > 1 faux
    CHECK_EQ(format_size_fr(1572864), std::string("1,5 Mo"));
    CHECK_EQ(format_size_fr(2097152), std::string("2 Mo"));

    // --- 2. mod_is_disabled (C# EndsWith ".disabled", insensible a la casse) ---
    CHECK(!mod_is_disabled("sodium.jar"));
    CHECK(mod_is_disabled("sodium.jar.disabled"));
    CHECK(mod_is_disabled("SODIUM.JAR.DISABLED"));
    CHECK(!mod_is_disabled("disabled.jar"));
    CHECK(!mod_is_disabled(".disabled")); // trop court : pas de nom de base
    CHECK(!mod_is_disabled(""));

    // --- 3. mod_base_name (C# fi.Name[..^".disabled".Length]) ---
    CHECK_EQ(mod_base_name("sodium.jar.disabled"), std::string("sodium.jar"));
    CHECK_EQ(mod_base_name("sodium.jar"), std::string("sodium.jar"));

    // --- 4. sort_mod_paths (C# OrderBy(f => f) : tri ordinal) ---
    {
        std::vector<std::string> v{"sodium.jar", "Lithium.jar",
                                   "ferrite-core.jar"};
        sort_mod_paths(v);
        CHECK_EQ(v[0], std::string("Lithium.jar"));
        CHECK_EQ(v[1], std::string("ferrite-core.jar"));
        CHECK_EQ(v[2], std::string("sodium.jar"));
    }

    // --- 5. resolve_log_file (C# FindLogFile) ---
    {
        const fs::path tmp = fs::temp_directory_path() / "tl-inst-detail-test";
        std::error_code ec;
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp / "inst1" / "logs", ec);
        fs::create_directories(tmp / "inst2" / "logs", ec);
        fs::create_directories(tmp / "inst3", ec);
        {
            std::ofstream out(tmp / "inst1" / "game-log.txt", std::ios::binary);
            out << "log";
        }
        {
            std::ofstream out(tmp / "inst1" / "logs" / "latest.log",
                              std::ios::binary);
            out << "latest";
        }
        {
            std::ofstream out(tmp / "inst2" / "logs" / "latest.log",
                              std::ios::binary);
            out << "latest";
        }
        // game-log.txt prioritaire sur logs/latest.log
        CHECK_EQ(resolve_log_file((tmp / "inst1").string()).value_or(""),
                 (tmp / "inst1" / "game-log.txt").string());
        // repli latest.log
        CHECK_EQ(resolve_log_file((tmp / "inst2").string()).value_or(""),
                 (tmp / "inst2" / "logs" / "latest.log").string());
        // rien -> nullopt (C# : null)
        CHECK(!resolve_log_file((tmp / "inst3").string()).has_value());
        fs::remove_all(tmp, ec);
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

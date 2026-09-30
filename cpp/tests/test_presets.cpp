// Tests des profils de lancement : lecture/ecriture dans le JSON
// d'instance, capture de l'etat des mods, et application (renommages).

#include "presets.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace tl::presets;
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
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static void touch(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary);
    o << "jar";
}

static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

int main() {
    // =====================================================================
    // 1. Serialisation dans le JSON d'instance
    // =====================================================================
    {
        json inst = json::object();
        CHECK(load(inst).empty());
        CHECK_EQ(active(inst), std::string(""));

        std::vector<Preset> v;
        Preset a;
        a.name = "Compétitif";
        a.ramGb = 4;
        a.jvmArgs = "-XX:+UseG1GC";
        a.hasMods = true;
        a.disabled = {"shaders.jar", "carte.jar"};
        Preset b;
        b.name = "Confort"; // sans mods captures ni surcharges
        v.push_back(a);
        v.push_back(b);
        store(inst, v);

        const auto back = load(inst);
        CHECK_EQ(back.size(), std::size_t{2});
        if (back.size() == 2) {
            CHECK_EQ(back[0].name, std::string("Compétitif"));
            CHECK_EQ(back[0].ramGb, 4);
            CHECK_EQ(back[0].jvmArgs, std::string("-XX:+UseG1GC"));
            CHECK(back[0].hasMods);
            CHECK_EQ(back[0].disabled.size(), std::size_t{2});
            CHECK(!back[1].hasMods);
            CHECK(back[1].disabled.empty());
        }

        set_active(inst, "Confort");
        CHECK_EQ(active(inst), std::string("Confort"));
        set_active(inst, "");
        CHECK_EQ(active(inst), std::string(""));
        CHECK(!inst.contains("ActivePreset")); // pas de cle vide qui traine

        // Une liste vide ne laisse pas de tableau derriere elle.
        store(inst, {});
        CHECK(!inst.contains("Presets"));

        // Entrees degradees : ignorees sans planter.
        inst["Presets"] = json::array({json::object(),          // sans nom
                                       json{{"Name", ""}},      // nom vide
                                       "pas un objet",
                                       json{{"Name", "Ok"}}});
        const auto tol = load(inst);
        CHECK_EQ(tol.size(), std::size_t{1});
        if (!tol.empty()) CHECK_EQ(tol[0].name, std::string("Ok"));
    }

    // =====================================================================
    // 2. Lecture du dossier de mods
    // =====================================================================
    const fs::path dir = fs::temp_directory_path() / "tl-presets-test" / "mods";
    std::error_code ec;
    fs::remove_all(dir.parent_path(), ec);
    touch(dir / "sodium.jar");
    touch(dir / "iris.jar.disabled");
    touch(dir / "carte.jar");
    touch(dir / "LISEZMOI.txt");          // pas un mod
    touch(dir / "config" / "truc.jar");   // sous-dossier : ignore

    {
        const auto all = all_mods(dir);
        CHECK_EQ(all.size(), std::size_t{3});
        CHECK(has(all, "sodium.jar"));
        CHECK(has(all, "iris.jar"));   // nom de BASE, pas .disabled
        CHECK(has(all, "carte.jar"));
        CHECK(!has(all, "LISEZMOI.txt"));

        const auto off = current_disabled(dir);
        CHECK_EQ(off.size(), std::size_t{1});
        CHECK(has(off, "iris.jar"));

        // Dossier inexistant : vide, pas de plantage.
        CHECK(all_mods(dir / "absent").empty());
        CHECK(current_disabled(dir / "absent").empty());
    }

    // =====================================================================
    // 3. Application : active et desactive ce qu'il faut
    // =====================================================================
    {
        const std::vector<std::string> known = {"sodium.jar", "iris.jar",
                                                "carte.jar"};
        // Profil « competitif » : on ne garde que sodium.
        const auto r = apply_mods(dir, {"iris.jar", "carte.jar"}, known);
        CHECK_EQ(r.disabled, 1); // carte passe off (iris l'etait deja)
        CHECK_EQ(r.enabled, 0);
        CHECK_EQ(r.failed, 0);
        CHECK(fs::exists(dir / "sodium.jar"));
        CHECK(fs::exists(dir / "iris.jar.disabled"));
        CHECK(fs::exists(dir / "carte.jar.disabled"));

        // Profil « confort » : tout actif.
        const auto r2 = apply_mods(dir, {}, known);
        CHECK_EQ(r2.enabled, 2);
        CHECK_EQ(r2.disabled, 0);
        CHECK(fs::exists(dir / "iris.jar"));
        CHECK(fs::exists(dir / "carte.jar"));

        // Rejouer le meme profil ne change plus rien.
        const auto r3 = apply_mods(dir, {}, known);
        CHECK_EQ(r3.enabled, 0);
        CHECK_EQ(r3.disabled, 0);
    }

    // =====================================================================
    // 4. Un mod installe APRES la capture n'est pas touche
    // =====================================================================
    {
        touch(dir / "nouveau.jar");
        const std::vector<std::string> known = {"sodium.jar", "iris.jar",
                                                "carte.jar"};
        // Le profil ne connait pas « nouveau.jar » : il doit rester ACTIF,
        // pas disparaitre en silence.
        const auto r = apply_mods(dir, {"iris.jar"}, known);
        CHECK_EQ(r.untouched, 1);
        CHECK(fs::exists(dir / "nouveau.jar"));
        CHECK(!fs::exists(dir / "nouveau.jar.disabled"));
        CHECK(fs::exists(dir / "iris.jar.disabled"));
    }

    // =====================================================================
    // 5. Dossier absent : aucun effet, aucun plantage
    // =====================================================================
    {
        const auto r = apply_mods(dir / "absent", {"x.jar"}, {"x.jar"});
        CHECK_EQ(r.enabled, 0);
        CHECK_EQ(r.disabled, 0);
        CHECK_EQ(r.failed, 0);
    }

    fs::remove_all(dir.parent_path(), ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

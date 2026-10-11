// Plugins en intégrité basse (V8, Windows).
//
// On ne se contente pas de vérifier qu'une option est posée : on lance un
// vrai processus en intégrité basse et on constate qu'il NE PEUT PAS écrire
// dans un dossier ordinaire de l'utilisateur, alors qu'il le peut dans un
// dossier étiqueté « Low ». Hors Windows, le test se contente de vérifier
// que l'option est sans effet.

#include "pipe_child.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
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

namespace {

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

#ifdef _WIN32
// Lance « cmd /c echo x> fichier » et attend sa fin. Rend le niveau
// d'intégrité observé pendant qu'il tournait.
long run_writer(const fs::path& target, bool low) {
    pipechild::Options o;
    o.program = "cmd.exe";
    // « ping » garde le processus en vie le temps de mesurer son niveau.
    // Pas de guillemets autour du chemin : cmd ne comprend pas l'échappement
    // de la ligne de commande, et le dossier temporaire n'a pas d'espace.
    o.args = {"/c", "ping -n 2 127.0.0.1 >nul & echo x>" + u8(target)};
    o.lowIntegrity = low;
    pipechild::Child c;
    std::string err;
    if (!c.start(o, &err)) {
        std::printf("  lancement : %s\n", err.c_str());
        return -2;
    }
    const long level = pipechild::integrity_level(c.pid());
    char buf[256];
    while (c.read_some(buf, sizeof(buf)) > 0) {
    }
    for (int i = 0; i < 100 && c.alive(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return level;
}
#endif

} // namespace

int main() {
#ifdef _WIN32
    const fs::path base = fs::temp_directory_path() / "tl_test_lowil";
    std::error_code ec;
    fs::remove_all(base, ec);
    const fs::path normal = base / "ordinaire";
    const fs::path labeled = base / "journaux";
    fs::create_directories(normal);
    fs::create_directories(labeled);

    // Témoin : sans l'option, intégrité moyenne (ou plus), écriture permise.
    const long medium = run_writer(normal / "temoin.txt", false);
    CHECK(medium >= 0x2000);
    CHECK(fs::exists(normal / "temoin.txt"));

    // Intégrité basse : le processus tourne bien au niveau bas…
    const long low = run_writer(normal / "interdit.txt", true);
    CHECK(low == 0x1000);
    if (low != 0x1000) std::printf("  niveau observé : 0x%lx\n", low);
    // … et ne peut PAS écrire dans un dossier ordinaire de l'utilisateur.
    CHECK(!fs::exists(normal / "interdit.txt"));

    // Dans un dossier étiqueté « Low », il le peut : c'est ainsi que les
    // plugins gardent leur journal d'erreurs.
    std::string err;
    CHECK(pipechild::allow_low_integrity_writes(u8(labeled), &err));
    if (!err.empty()) std::printf("  étiquette : %s\n", err.c_str());
    run_writer(labeled / "autorise.txt", true);
    CHECK(fs::exists(labeled / "autorise.txt"));

    fs::remove_all(base, ec);
#else
    std::string err;
    CHECK(pipechild::allow_low_integrity_writes("/tmp", &err));
    CHECK(pipechild::integrity_level(1) == -1);
#endif
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

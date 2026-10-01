#include "jvmwarm.hpp"

#include "game_installer.hpp" // runtime_root
#include "game_launcher.hpp"  // find_java, detect_java_major

#include <algorithm>
#include <chrono>
#include <fstream>
#include <mutex>
#include <system_error>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace tl::jvmwarm {

namespace {

// Plafonds. Lire tout le dossier des bibliotheques sur une installation
// fournie reviendrait a inonder le cache du systeme avec des fichiers dont
// la partie ne se servira pas — et a evincer ceux dont elle se sert.
constexpr int kMaxFiles = 120;
constexpr long long kMaxBytes = 192LL * 1024 * 1024;
// On ne lit que le DEBUT de chaque jar : l'index d'une archive zip est en
// fin de fichier, mais ce qui compte ici est d'amener les pages en memoire
// et de faire tourner le prefetch du systeme, pas de tout charger.
constexpr long long kHeadBytes = 512 * 1024;

struct Ctx {
    std::mutex m;
    std::thread th;
    std::atomic<bool> cancel{false};
    State st;
    std::string wanted; // instance demandee la plus recente
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

// Les plus gros jars sous `root`, les plus gros d'abord.
std::vector<fs::path> big_jars(const fs::path& root, int limit) {
    struct Item {
        fs::path p;
        long long size;
    };
    std::vector<Item> items;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return {};
    for (auto it = fs::recursive_directory_iterator(
             root, fs::directory_options::skip_permission_denied, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string n = it->path().filename().string();
        if (n.size() < 4 || n.compare(n.size() - 4, 4, ".jar") != 0) continue;
        const auto sz = static_cast<long long>(fs::file_size(it->path(), ec));
        if (ec) continue;
        items.push_back({it->path(), sz});
        // Garde-fou : une arborescence anormalement grande ne doit pas
        // faire tourner ce balayage indefiniment.
        if (items.size() > 4000) break;
    }
    std::sort(items.begin(), items.end(),
              [](const Item& a, const Item& b) { return a.size > b.size; });
    std::vector<fs::path> out;
    for (int i = 0; i < limit && i < static_cast<int>(items.size()); ++i)
        out.push_back(items[static_cast<size_t>(i)].p);
    return out;
}

void run(const std::string& instId) {
    auto& c = ctx();
    const auto t0 = std::chrono::steady_clock::now();
    State st;
    st.warming = true;
    st.instanceId = instId;

    // 1. Java. C'est le poste le plus cher, et le plus sûrement gagné :
    // find_java met son résultat en cache, donc le lancement ne refera pas
    // le balayage. On demande 17 puis 8 : la plupart des versions récentes
    // veulent 17 ou 21, les anciennes 8, et un Java 17 satisfait aussi une
    // instance qui n'en demande que 8.
    if (!c.cancel.load()) {
        auto java = find_java(17);
        if (!java) java = find_java(8);
        if (java) {
            st.javaPath = *java;
            st.javaMajor = detect_java_major(*java);
        }
    }

    // 2. Cache disque. Annulable entre chaque fichier : l'utilisateur peut
    // changer d'instance ou cliquer sur Jouer à tout moment.
    if (!c.cancel.load()) {
        std::vector<fs::path> files =
            big_jars(runtime_root() / "versions", kMaxFiles / 3);
        const auto libs = big_jars(runtime_root() / "libraries",
                                   kMaxFiles - static_cast<int>(files.size()));
        files.insert(files.end(), libs.begin(), libs.end());

        std::vector<char> buf(64 * 1024);
        for (const auto& f : files) {
            if (c.cancel.load()) break;
            if (st.bytesTouched >= kMaxBytes) break;
            std::ifstream in(f, std::ios::binary);
            if (!in) continue;
            long long read = 0;
            while (read < kHeadBytes && in) {
                in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
                const auto got = in.gcount();
                if (got <= 0) break;
                read += got;
                if (c.cancel.load()) break;
            }
            st.bytesTouched += read;
            ++st.filesTouched;
        }
    }

    st.ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count();
    st.warming = false;
    st.ready = !c.cancel.load();
    if (!st.ready) {
        st.detail = "Préchauffage interrompu.";
    } else if (st.javaPath.empty()) {
        st.detail =
            "Aucun Java installé n'a été trouvé : il sera téléchargé au "
            "premier lancement.";
    } else {
        st.detail = "Java " + std::to_string(st.javaMajor) + " repéré, " +
                    std::to_string(st.filesTouched) +
                    " fichier(s) mis en cache (" +
                    std::to_string((st.bytesTouched + 524288) / 1048576) +
                    " Mo) en " + std::to_string(st.ms) + " ms.";
    }

    std::lock_guard<std::mutex> lk(c.m);
    c.st = std::move(st);
}

} // namespace

void request(const nlohmann::json& inst) {
    const std::string id = inst.value("Id", "");
    if (id.empty()) return;
    auto& c = ctx();

    std::thread old;
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (c.wanted == id && (c.st.warming || c.st.ready)) return;
        c.wanted = id;
        // Préchauffer l'instance qu'on vient de quitter ne sert à rien.
        if (c.th.joinable()) {
            c.cancel.store(true);
            old = std::move(c.th);
        }
        c.st = State{};
        c.st.warming = true;
        c.st.instanceId = id;
    }
    if (old.joinable()) old.join();

    c.cancel.store(false);
    std::lock_guard<std::mutex> lk(c.m);
    c.th = std::thread([id] { run(id); });
}

State state() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    return c.st;
}

void stop() {
    auto& c = ctx();
    c.cancel.store(true);
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(c.m);
        th = std::move(c.th);
    }
    if (th.joinable()) th.join();
}

} // namespace tl::jvmwarm

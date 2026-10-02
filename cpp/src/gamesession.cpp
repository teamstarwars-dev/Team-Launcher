#include "gamesession.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line

#include <algorithm>
#include <chrono>
#include <ctime>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#else
#include <csignal>
#endif

namespace fs = std::filesystem;

namespace tl::gamesession {

namespace {

long long now_unix() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string iso_local(long long unixTime) {
    const std::time_t t = static_cast<std::time_t>(unixTime);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmv);
    return buf;
}

// Le processus tourne-t-il encore ? On ne se contente PAS de l'existence
// du PID : les numéros sont réutilisés, et compter le temps de jeu d'un
// processus qui n'a rien à voir serait pire que de ne rien compter. On
// vérifie donc que sa date de création correspond à celle qu'on a notée.
bool still_running(unsigned long pid, long long startedUnix) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    FILETIME creation{}, exitT{}, kernelT{}, userT{};
    bool ok = false;
    if (GetProcessTimes(h, &creation, &exitT, &kernelT, &userT)) {
        // FILETIME : intervalles de 100 ns depuis 1601. 11644473600 est
        // l'écart avec l'époque Unix, en secondes.
        ULARGE_INTEGER u{};
        u.LowPart = creation.dwLowDateTime;
        u.HighPart = creation.dwHighDateTime;
        const long long created =
            static_cast<long long>(u.QuadPart / 10000000ULL) - 11644473600LL;
        // Deux minutes de tolérance : le marqueur est écrit peu après le
        // démarrage du jeu, pas à la microseconde.
        ok = std::llabs(created - startedUnix) < 120;
    }
    CloseHandle(h);
    return ok;
#else
    // Sous Linux, /proc/<pid>/stat donne l'heure de démarrage en ticks
    // depuis le boot. Plus simple et suffisant ici : l'existence du
    // processus, plus un contrôle que son dossier /proc est au moins
    // aussi vieux que notre marqueur.
    if (::kill(static_cast<pid_t>(pid), 0) != 0) return false;
    std::error_code ec;
    const fs::path p = fs::path("/proc") / std::to_string(pid);
    if (!fs::exists(p, ec)) return false;
    const auto t = file_time_to_unix(fs::last_write_time(p, ec));
    if (ec) return true; // dans le doute, on ne compte rien
    return std::llabs(t - startedUnix) < 120;
#endif
}

} // namespace

fs::path marker_path() { return DataStore::dir() / "session.json"; }

bool begin(const Session& s) {
    if (s.instanceId.empty()) return false;
    std::error_code ec;
    fs::create_directories(marker_path().parent_path(), ec);
    std::ofstream out(marker_path(), std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << nlohmann::json{{"instanceId", s.instanceId},
                          {"pid", s.pid},
                          {"startedUnix", s.startedUnix}}
               .dump(2);
    return static_cast<bool>(out);
}

void clear() {
    std::error_code ec;
    fs::remove(marker_path(), ec);
}

std::optional<Session> current() {
    std::error_code ec;
    if (!fs::is_regular_file(marker_path(), ec)) return std::nullopt;
    // Le contenu est lu PUIS le flux refermé, avant toute tentative de
    // suppression : sous Windows, effacer un fichier dont on tient
    // encore une poignée échoue en silence, et un marqueur corrompu
    // serait relu — et re-raté — à chaque démarrage, indéfiniment.
    std::string body;
    {
        std::ifstream in(marker_path(), std::ios::binary);
        if (!in) return std::nullopt;
        body.assign((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    }
    try {
        const auto j = nlohmann::json::parse(body);
        Session s;
        s.instanceId = j.value("instanceId", std::string{});
        s.pid = j.value("pid", 0UL);
        s.startedUnix = j.value("startedUnix", 0LL);
        if (s.instanceId.empty() || s.startedUnix <= 0) return std::nullopt;
        return s;
    } catch (const std::exception&) {
        // Marqueur illisible : on le jette plutôt que de le traîner.
        clear();
        return std::nullopt;
    }
}

long long last_activity_unix(const fs::path& gameDir) {
    std::error_code ec;
    long long best = 0;
    auto consider = [&](const fs::path& p) {
        if (!fs::exists(p, ec)) return;
        const auto t = file_time_to_unix(fs::last_write_time(p, ec));
        if (!ec && t > best) best = t;
    };
    // Le journal du jeu est le témoin le plus fidèle : Minecraft y écrit
    // en continu. Les sauvegardes de monde suivent, à chaque autosave.
    consider(gameDir / "logs" / "latest.log");
    consider(gameDir / "game-log.txt");
    if (fs::is_directory(gameDir / "saves", ec))
        for (const auto& w : fs::directory_iterator(gameDir / "saves", ec)) {
            if (ec) break;
            consider(w.path() / "level.dat");
            consider(w.path() / "session.lock");
        }
    // Les rapports de crash datent de la fin, par construction.
    if (fs::is_directory(gameDir / "crash-reports", ec))
        for (const auto& c : fs::directory_iterator(gameDir / "crash-reports", ec)) {
            if (ec) break;
            consider(c.path());
        }
    return best;
}

long long clamp_duration(long long startedUnix, long long endUnix,
                         long long maxSeconds) {
    if (startedUnix <= 0 || endUnix <= startedUnix) return 0;
    const long long d = endUnix - startedUnix;
    return d > maxSeconds ? maxSeconds : d;
}

Recovered reconcile() {
    Recovered r;
    const auto s = current();
    if (!s) return r;
    r.instanceId = s->instanceId;

    // La partie tourne encore (launcher fermé, jeu toujours ouvert) : on
    // ne touche à rien. On réessaiera au prochain démarrage, quand elle
    // sera vraiment finie.
    if (still_running(s->pid, s->startedUnix)) {
        r.stillRunning = true;
        return r;
    }

    const fs::path gameDir = DataStore::instancesRoot() / s->instanceId;
    long long end = last_activity_unix(gameDir);
    // Aucun fichier exploitable : on ne devine pas. Le marqueur part
    // quand même, sinon il resterait là indéfiniment.
    if (end <= 0) {
        clear();
        return r;
    }
    const long long now = now_unix();
    if (end > now) end = now; // horloge reculée, ou fichier daté du futur
    const long long secs = clamp_duration(s->startedUnix, end);
    clear();
    if (secs <= 0) return r;

    for (auto& e : DataStore::settings.instances) {
        if (!e.is_object() || e.value("Id", "") != s->instanceId) continue;
        e["PlaySeconds"] = e.value("PlaySeconds", 0LL) + secs;
        e["LastPlayed"] = iso_local(end);
        r.instanceName = e.value("Name", s->instanceId);
        r.applied = true;
        r.seconds = secs;
        break;
    }
    if (r.applied) {
        DataStore::save();
        log_line("Session précédente rattrapée : " + std::to_string(secs / 60) +
                 " min ajoutées à « " + r.instanceName +
                 " » (fin estimée d'après les fichiers du jeu).");
    }
    return r;
}

} // namespace tl::gamesession

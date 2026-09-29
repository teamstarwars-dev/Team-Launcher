#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
// Etape 5 (Linux) : nom d'OS depuis /etc/os-release (repli "Linux").
#include <fstream>
#endif

#include "telemetry.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // find_java
#include "http_win.hpp"
#include "maintenance.hpp"   // updates::current_version

#include <condition_variable>
#include <ctime>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

using nlohmann::json;

namespace tl::telemetry {

namespace {

struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::pair<std::string, std::string>> items; // url, payload JSON
    bool stopping = false;
    bool started = false;
    std::thread th;
};

Queue& q() {
    static Queue inst;
    return inst;
}

void worker() {
    for (;;) {
        std::pair<std::string, std::string> item;
        {
            std::unique_lock<std::mutex> lk(q().m);
            q().cv.wait(lk, [] { return q().stopping || !q().items.empty(); });
            if (q().items.empty()) return; // stopping et file vide
            item = std::move(q().items.front());
            q().items.pop_front();
        }
        // Echec silencieux (webhook incorrect, Discord indisponible...).
        http::post_string(item.first, item.second, "application/json");
    }
}

void enqueue(const std::string& url, const std::string& payload) {
    std::lock_guard<std::mutex> lk(q().m);
    if (q().stopping) return;
    q().items.emplace_back(url, payload);
    if (!q().started) {
        q().started = true;
        q().th = std::thread(worker);
    }
    q().cv.notify_one();
}

std::string now_fr() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &tmv);
    return buf;
}

std::string now_iso8601_utc() {
    const std::time_t t = std::time(nullptr);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

std::string truncate_tail(const std::string& s, size_t n) {
    return s.size() > n ? "..." + s.substr(s.size() - n) : s;
}

std::string truncate_head(const std::string& s, size_t n) {
    return s.size() > n ? s.substr(0, n) + "..." : s;
}

void send_embed(const std::string& title, const std::string& description,
                int color) {
    const std::string url = DataStore::settings.discordTelemetryWebhook;
    if (!DataStore::settings.telemetryEnabled || url.empty()) return;

    const json payload = {
        {"embeds",
         json::array({{{"title", "Team Launcher — " + title},
                       {"description", truncate_head(description, 4000)},
                       {"color", color},
                       {"footer",
                        {{"text", std::string("Team Launcher v") +
                                      updates::current_version()}}},
                       {"timestamp", now_iso8601_utc()}}})}};
    enqueue(url, payload.dump());
}

// Bloc ``` du C# : champs alignes.
std::string inst_block(const json& inst, const std::string& extra = {}) {
    std::ostringstream s;
    s << "```\n"
      << "Instance  : " << inst.value("Name", "") << " (" << inst.value("Id", "")
      << ")\n"
      << "Loader    : " << inst.value("Loader", "") << "\n"
      << "Version   : Minecraft " << inst.value("McVersion", "") << "\n"
      << extra << "Date      : " << now_fr() << "\n"
      << "Lancements: " << inst.value("Launches", 0) << "\n"
      << "```\n";
    return s.str();
}

} // namespace

bool enabled() {
    return DataStore::settings.telemetryEnabled &&
           !DataStore::settings.discordTelemetryWebhook.empty();
}

void report_crash(const json& inst, int exitCode, const std::string& gameLogTail) {
    if (!enabled()) return;
    std::ostringstream s;
    s << "**Crash Minecraft** — " << inst.value("Name", "") << "\n"
      << inst_block(inst, "Exit code : " + std::to_string(exitCode) + "\n");
    if (!gameLogTail.empty()) {
        s << "**Dernières lignes du log :**\n```\n"
          << truncate_tail(gameLogTail, 1800) << "\n```\n";
    }
    send_embed("Crash Minecraft", s.str(), 0xE74C3C);
}

void report_launch(const json& inst) {
    if (!enabled()) return;
    const int ram = inst.value("MaxRamGb", 0);
    std::ostringstream s;
    s << "**Lancement** — " << inst.value("Name", "") << "\n"
      << inst_block(inst, "RAM       : " +
                              (ram > 0 ? std::to_string(ram) + " Go"
                                       : std::string("globale")) +
                              "\n");
    send_embed("Lancement", s.str(), 0x3498DB);
}

void report_instance_deleted(const json& inst) {
    if (!enabled()) return;
    std::ostringstream s;
    s << "**Instance supprimée** — " << inst.value("Name", "") << "\n"
      << inst_block(inst);
    send_embed("Instance supprimée", s.str(), 0x9B59B6);
}

void report_startup() {
    if (!enabled()) return;
    // Diagnostic Java hors thread UI : c'est le worker qui envoie, mais la
    // detection Java est faite ici (cache partage, appel rapide).
    const bool j8 = find_java(8).has_value();
    const bool j17 = find_java(17).has_value();
    const bool j21 = find_java(21).has_value();

#ifdef _WIN32
    OSVERSIONINFOEXW osv{};
    osv.dwOSVersionInfoSize = sizeof(osv);
#else
    const std::string osLabel = [] {
        std::ifstream os("/etc/os-release");
        std::string line;
        const std::string key = "PRETTY_NAME=";
        while (std::getline(os, line)) {
            if (line.rfind(key, 0) != 0) continue;
            std::string v = line.substr(key.size());
            if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
            if (!v.empty()) return v;
        }
        return std::string("Linux");
    }();
#endif

    const auto& st = DataStore::settings;
    std::ostringstream s;
    s << "**Démarrage du Launcher**\n```\n"
      << "Version     : " << updates::current_version() << "\n"
#ifdef _WIN32
      << "OS          : Windows\n"
#else
      << "OS          : " << osLabel << "\n"
#endif
      << "64-bit      : " << (sizeof(void*) == 8 ? "True" : "False") << "\n"
      << "Instances   : "
      << (st.instances.is_array() ? st.instances.size() : size_t{0}) << "\n"
      << "Compte      : " << st.accountMode << "\n"
      << "Java custom : " << (st.javaPath.empty() ? "non" : "oui") << "\n"
      << "Date        : " << now_fr() << "\n"
      << "```\n"
      << "**Java** : 8=" << (j8 ? "oui" : "non") << " 17=" << (j17 ? "oui" : "non")
      << " 21=" << (j21 ? "oui" : "non") << "\n";
    send_embed("Démarrage", s.str(), 0x2ECC71);
}

void stop() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(q().m);
        q().stopping = true;
        th = std::move(q().th);
    }
    q().cv.notify_all();
    if (th.joinable()) th.join();
}

} // namespace tl::telemetry

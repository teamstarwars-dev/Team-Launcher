#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "admin.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line, total_ram_mb
#include "http_win.hpp"
#include "maintenance.hpp"   // updates::current_version

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using nlohmann::json;
using namespace std::chrono_literals;

namespace tl::admin {

namespace {

constexpr auto kHeartbeat = 5min; // Interval du Timer C#

struct Ctx {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool running = false;
    bool stopping = false;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

// Valeur chaine du registre, "" si absente.
std::string reg_string(HKEY root, const wchar_t* key, const wchar_t* value) {
    wchar_t buf[512] = L"";
    DWORD size = sizeof(buf);
    DWORD type = 0;
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, &type, buf, &size) != ERROR_SUCCESS)
        return {};
    return wide_to_utf8(buf);
}

// Dernier McVersion joue (C# : instance au LastPlayed le plus recent).
std::string last_mc_version() {
    const auto& arr = DataStore::settings.instances;
    if (!arr.is_array()) return {};
    std::string bestDate, bestVer;
    for (const auto& e : arr) {
        if (!e.is_object()) continue;
        const std::string d = e.value("LastPlayed", std::string{});
        // "0001-01-01..." = jamais lancee (DateTime.MinValue du C#)
        if (d.size() < 10 || d.compare(0, 10, "0001-01-01") == 0) continue;
        if (d > bestDate) { // ISO 8601 : ordre lexicographique = chronologique
            bestDate = d;
            bestVer = e.value("McVersion", std::string{});
        }
    }
    return bestVer;
}

std::string base_url() {
    std::string u = DataStore::settings.adminServerUrl;
    while (!u.empty() && u.back() == '/') u.pop_back();
    return u;
}

// Envoi non bloquant : un thread detache par message serait couteux, on
// reutilise le thread de battement quand il tourne, sinon envoi direct depuis
// l'appelant (les appels ponctuels sont deja hors du fil UI).
void post(const std::string& path, const json& payload) {
    const std::string url = base_url();
    if (url.empty()) return;
    http::post_string(url + "/" + path, payload.dump(), "application/json");
}

} // namespace

bool enabled() {
    return DataStore::settings.adminTelemetryEnabled &&
           !DataStore::settings.adminServerUrl.empty();
}

std::string installation_id() {
    if (DataStore::settings.installationId.empty()) {
        // Le C# posait un Guid par defaut dans le modele ; ici le champ est
        // vide tant qu'il n'a pas servi, on le genere a la demande.
        DataStore::settings.installationId = new_guid();
        DataStore::save();
    }
    return DataStore::settings.installationId;
}

Machine machine_info() {
    Machine m;

    wchar_t host[MAX_COMPUTERNAME_LENGTH + 1] = L"";
    DWORD hn = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(host, &hn)) m.hostname = wide_to_utf8(host);

    // Version de Windows : le registre donne le vrai numero de build, que
    // GetVersionEx masque depuis Windows 8.1 sans manifeste.
    const wchar_t* kCurVer = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::string product = reg_string(HKEY_LOCAL_MACHINE, kCurVer, L"ProductName");
    const std::string build = reg_string(HKEY_LOCAL_MACHINE, kCurVer, L"CurrentBuild");
    const std::string display = reg_string(HKEY_LOCAL_MACHINE, kCurVer, L"DisplayVersion");
    m.osVersion = product;
    // ProductName reste bloque sur « Windows 10 » meme sous Windows 11 : c'est
    // le build qui fait foi (>= 22000). Le C# avait le meme travers via
    // Environment.OSVersion (« 10.0.26200 ») — la donnee remontee etait donc
    // inexploitable pour distinguer les deux systemes.
    if (!build.empty() && std::atoi(build.c_str()) >= 22000 &&
        m.osVersion.find("Windows 10") == 0)
        m.osVersion.replace(0, 10, "Windows 11");
    if (!display.empty()) m.osVersion += " " + display;
    if (!build.empty()) m.osVersion += " (build " + build + ")";

    m.cpuName = reg_string(HKEY_LOCAL_MACHINE,
                           L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                           L"ProcessorNameString");

    // Carte graphique principale (celle attachee au bureau).
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof(dd);
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
        if (dd.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) {
            m.gpuName = wide_to_utf8(dd.DeviceString);
            break;
        }
        if (m.gpuName.empty()) m.gpuName = wide_to_utf8(dd.DeviceString);
        dd.cb = sizeof(dd);
    }

    m.ramMb = total_ram_mb();
    if (m.ramMb < 0) m.ramMb = 0;
    return m;
}

json heartbeat_payload() {
    const Machine m = machine_info();
    const auto& arr = DataStore::settings.instances;
    // Memes noms de champs que le C# : le serveur d'admin ne change pas.
    return json{{"instance_id", installation_id()},
                {"hostname", m.hostname},
                {"os_version", m.osVersion},
                {"launcher_version", updates::current_version()},
                {"ram_mb", m.ramMb},
                {"cpu_name", m.cpuName},
                {"gpu_name", m.gpuName},
                {"mc_version", last_mc_version()},
                {"instance_count", arr.is_array() ? arr.size() : size_t{0}}};
}

void send_heartbeat() {
    if (!enabled()) return;
    post("api/telemetry/heartbeat", heartbeat_payload());
}

void send_event(const std::string& eventType, const json& data) {
    if (!enabled()) return;
    post("api/telemetry/event", json{{"instance_id", installation_id()},
                                     {"event_type", eventType},
                                     {"event_data", data}});
}

void report_error(const std::string& source, const std::string& message,
                  const std::string& stackTrace, const std::string& mcVersion,
                  const std::string& loader) {
    if (!enabled()) return;
    post("api/telemetry/error", json{{"instance_id", installation_id()},
                                     {"level", "error"},
                                     {"source", source},
                                     {"message", message},
                                     {"stack_trace", stackTrace},
                                     {"mc_version", mcVersion},
                                     {"loader", loader}});
}

void start() {
    if (!enabled()) return;
    {
        std::lock_guard<std::mutex> lk(ctx().m);
        if (ctx().running) return;
        if (ctx().th.joinable()) ctx().th.join();
        ctx().running = true;
        ctx().stopping = false;
    }
    ctx().th = std::thread([] {
        log_line("[Admin] télémétrie d'administration active.");
        send_heartbeat();
        send_event("launcher_start",
                   json{{"version", updates::current_version()}});
        for (;;) {
            std::unique_lock<std::mutex> lk(ctx().m);
            // wait_for rend la main immediatement au shutdown : pas d'attente
            // de 5 minutes a la fermeture du launcher.
            if (ctx().cv.wait_for(lk, kHeartbeat, [] { return ctx().stopping; }))
                return;
            lk.unlock();
            send_heartbeat();
        }
    });
}

void stop() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(ctx().m);
        if (!ctx().running) return;
        ctx().stopping = true;
        ctx().running = false;
        th = std::move(ctx().th);
    }
    ctx().cv.notify_all();
    if (th.joinable()) th.join();
}

} // namespace tl::admin

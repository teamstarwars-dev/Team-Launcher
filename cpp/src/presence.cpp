#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include "presence.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

using nlohmann::json;
using namespace std::chrono_literals;

namespace tl::presence {

namespace {

constexpr const char* kSiteUrl = "https://teamstarwars-dev.github.io/Team-Luncher-/";
constexpr auto kRetryDelay = 20s; // chien de garde du C#

void plog(const std::string& s) { log_line("[Presence] " + s); }

// --- Etat souhaite, pose par l'UI, consomme par le thread ----------------
struct Desired {
    bool inGame = false;
    std::string instName, loader, mcVersion, server, cityName, cityOwner;
    long long instPlaySeconds = 0;
    long long totalPlaySeconds = 0;
    long long sessionStart = 0; // epoch s, 0 = pas de chrono
};

struct Ctx {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool running = false;
    bool stopping = false;
    bool dirty = false;
    bool connected = false;
    Desired want;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// --- Tube nomme ------------------------------------------------------------

class Pipe {
public:
    ~Pipe() { close(); }

    bool open() {
        close();
        for (int i = 0; i < 10; ++i) {
            wchar_t path[64];
            std::swprintf(path, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                h_ = h;
                return true;
            }
        }
        return false;
    }

    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
    }

    bool valid() const { return h_ != INVALID_HANDLE_VALUE; }

    bool write_frame(std::uint32_t opcode, const std::string& payload) {
        if (!valid()) return false;
        std::string buf;
        buf.resize(8 + payload.size());
        const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        // Entiers 32 bits little-endian (x86/x64 : copie directe).
        std::memcpy(buf.data(), &opcode, 4);
        std::memcpy(buf.data() + 4, &len, 4);
        std::memcpy(buf.data() + 8, payload.data(), payload.size());

        DWORD written = 0;
        return WriteFile(h_, buf.data(), static_cast<DWORD>(buf.size()), &written,
                         nullptr) &&
               written == buf.size();
    }

    // Lit une trame si disponible avant expiration. false = rien / lien mort.
    bool read_frame(std::uint32_t* opcode, std::string* payload, int timeoutMs) {
        if (!valid()) return false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        DWORD avail = 0;
        for (;;) {
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return false;
            if (avail >= 8) break;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(50ms);
        }
        char head[8];
        DWORD got = 0;
        if (!ReadFile(h_, head, 8, &got, nullptr) || got != 8) return false;
        std::uint32_t op = 0, len = 0;
        std::memcpy(&op, head, 4);
        std::memcpy(&len, head + 4, 4);
        // Garde-fou : une trame Discord depasse rarement quelques Ko.
        if (len > 1u << 20) return false;
        std::string body(len, '\0');
        if (len) {
            DWORD readTotal = 0;
            while (readTotal < len) {
                DWORD n = 0;
                if (!ReadFile(h_, body.data() + readTotal, len - readTotal, &n,
                              nullptr) ||
                    n == 0)
                    return false;
                readTotal += n;
            }
        }
        if (opcode) *opcode = op;
        if (payload) *payload = std::move(body);
        return true;
    }

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
};

std::string nonce() {
    static std::atomic<unsigned> n{0};
    return std::to_string(now_unix()) + "-" + std::to_string(++n);
}

std::string format_hours(long long seconds) {
    const long long h = seconds / 3600;
    const long long m = (seconds % 3600) / 60;
    if (h >= 1) return std::to_string(h) + " h " + std::to_string(m) + " min";
    return std::to_string(m) + " min";
}

// Construit la charge SET_ACTIVITY a partir de l'etat souhaite.
json build_activity(const Desired& d) {
    json assets = {{"large_image", "logo"}, {"large_text", "Team Launcher"}};
    json activity;

    if (d.inGame) {
        activity["details"] = "Joue à " + d.instName;
        if (!d.cityName.empty())
            activity["state"] = d.cityName + " — ville de " + d.cityOwner;
        else if (!d.server.empty())
            activity["state"] = "En multijoueur : " + d.server;
        else
            activity["state"] = "Team Launcher";
        if (d.sessionStart > 0) activity["timestamps"] = {{"start", d.sessionStart}};

        const long long totalHours = d.instPlaySeconds / 3600;
        std::string small = d.loader + " • Minecraft " + d.mcVersion;
        if (totalHours > 0) small += " • " + std::to_string(totalHours) + " h";
        assets["small_image"] = "logo";
        assets["small_text"] = small;
    } else {
        activity["details"] = "Dans le launcher";
        activity["state"] =
            "Temps de jeu total : " + format_hours(d.totalPlaySeconds);
    }

    activity["assets"] = assets;
    activity["buttons"] =
        json::array({{{"label", "🌐 Visiter le site"}, {"url", kSiteUrl}}});
    return activity;
}

void worker() {
    Pipe pipe;
    bool handshaked = false;
    auto lastAttempt = std::chrono::steady_clock::now() - kRetryDelay;

    for (;;) {
        Desired want;
        bool stop = false, dirty = false;
        {
            std::unique_lock<std::mutex> lk(ctx().m);
            ctx().cv.wait_for(lk, 2s, [] { return ctx().stopping || ctx().dirty; });
            stop = ctx().stopping;
            dirty = ctx().dirty;
            ctx().dirty = false;
            want = ctx().want;
        }
        if (stop) break;

        // --- Connexion / reconnexion (chien de garde 20 s) ---
        if (!handshaked) {
            if (std::chrono::steady_clock::now() - lastAttempt < kRetryDelay) continue;
            lastAttempt = std::chrono::steady_clock::now();
            const std::string appId = DataStore::settings.discordAppId;
            if (appId.empty()) continue;
            if (!pipe.open()) {
                plog("connexion échouée (Discord est-il ouvert ?).");
                continue;
            }
            const json hs = {{"v", 1}, {"client_id", appId}};
            if (!pipe.write_frame(0, hs.dump())) {
                plog("poignée de main impossible.");
                pipe.close();
                continue;
            }
            std::uint32_t op = 0;
            std::string body;
            if (!pipe.read_frame(&op, &body, 3000)) {
                plog("pas de réponse à la poignée de main.");
                pipe.close();
                continue;
            }
            handshaked = true;
            {
                std::lock_guard<std::mutex> lk(ctx().m);
                ctx().connected = true;
            }
            // La trame READY porte l'utilisateur connecte : on le journalise
            // comme le C#, ce qui confirme au passage que la reponse est bien
            // du JSON exploitable et pas un simple accuse de reception.
            std::string who;
            try {
                const json r = json::parse(body);
                if (auto d = r.find("data"); d != r.end() && d->is_object())
                    if (auto u = d->find("user"); u != d->end() && u->is_object())
                        who = " (user=" + u->value("username", std::string{}) +
                              ", id=" + u->value("id", std::string{}) + ")";
            } catch (const json::exception&) {
                // Reponse inattendue : sans importance, le lien fonctionne.
            }
            plog("connecté" + who + ".");
            dirty = true; // pousser l'etat courant des la connexion
        }

        if (!dirty) {
            // Draine les trames entrantes ; leur absence ne dit rien, mais un
            // echec de lecture signale un lien coupe.
            std::uint32_t op = 0;
            std::string body;
            if (pipe.read_frame(&op, &body, 0) && op == 2) { // CLOSE
                plog("connexion fermée par Discord (le chien de garde relance).");
                pipe.close();
                handshaked = false;
                std::lock_guard<std::mutex> lk(ctx().m);
                ctx().connected = false;
            }
            continue;
        }

        const json frame = {{"cmd", "SET_ACTIVITY"},
                            {"nonce", nonce()},
                            {"args",
                             {{"pid", static_cast<int>(GetCurrentProcessId())},
                              {"activity", build_activity(want)}}}};
        if (!pipe.write_frame(1, frame.dump())) {
            plog("envoi impossible, lien perdu.");
            pipe.close();
            handshaked = false;
            std::lock_guard<std::mutex> lk(ctx().m);
            ctx().connected = false;
        }
    }

    // Efface la presence avant de partir (C# ClearPresence).
    if (handshaked) {
        const json clear = {{"cmd", "SET_ACTIVITY"},
                            {"nonce", nonce()},
                            {"args",
                             {{"pid", static_cast<int>(GetCurrentProcessId())},
                              {"activity", nullptr}}}};
        pipe.write_frame(1, clear.dump());
    }
    pipe.close();
    std::lock_guard<std::mutex> lk(ctx().m);
    ctx().connected = false;
}

long long total_playtime() {
    long long total = 0;
    const auto& arr = DataStore::settings.instances;
    if (arr.is_array())
        for (const auto& e : arr)
            if (e.is_object()) total += e.value("PlaySeconds", 0LL);
    return total;
}

void publish(const Desired& d) {
    if (!enabled()) return;
    init();
    std::lock_guard<std::mutex> lk(ctx().m);
    ctx().want = d;
    ctx().dirty = true;
    ctx().cv.notify_all();
}

} // namespace

bool enabled() {
    return DataStore::settings.discordEnabled &&
           !DataStore::settings.discordAppId.empty();
}

bool connected() {
    std::lock_guard<std::mutex> lk(ctx().m);
    return ctx().connected;
}

void init() {
    if (!enabled()) return;
    std::lock_guard<std::mutex> lk(ctx().m);
    if (ctx().running) return;
    ctx().running = true;
    ctx().stopping = false;
    ctx().th = std::thread(worker);
}

void set_launcher() {
    Desired d;
    d.inGame = false;
    d.totalPlaySeconds = total_playtime();
    publish(d);
}

void set_game(const json& inst, const std::string& server) {
    Desired d;
    d.inGame = true;
    d.instName = inst.value("Name", "Minecraft");
    d.loader = inst.value("Loader", "Vanilla");
    d.mcVersion = inst.value("McVersion", "?");
    d.instPlaySeconds = inst.value("PlaySeconds", 0LL);
    d.server = server;
    d.sessionStart = now_unix();

    // Ville RP de la team correspondant a l'hote rejoint (quirk C# conserve :
    // comparaison sur l'hote seul, port ignore).
    if (!server.empty()) {
        const std::string host = server.substr(0, server.find(':'));
        const auto& cities = DataStore::settings.cities;
        if (cities.is_array())
            for (const auto& c : cities) {
                if (!c.is_object()) continue;
                const std::string addr = c.value("Address", std::string{});
                const std::string h = addr.substr(0, addr.find(':'));
                if (_stricmp(h.c_str(), host.c_str()) == 0) {
                    d.cityName = c.value("Name", std::string{});
                    d.cityOwner = c.value("Owner", std::string{});
                    break;
                }
            }
    }
    publish(d);
}

void shutdown() {
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

void reload() {
    shutdown();
    if (enabled()) {
        init();
        set_launcher();
    }
}

} // namespace tl::presence

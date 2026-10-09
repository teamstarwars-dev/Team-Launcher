#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
// Etape 5 (Linux) : IPC Discord sur socket Unix $XDG_RUNTIME_DIR/discord-ipc-N
// (meme format de trame que le tube nomme Windows).
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <strings.h> // strcasecmp
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h> // close, getpid, readlink
#endif

#include "presence.hpp"

#include "datastore.hpp"
#include "social.hpp" // presence_active : le SDK porte la presence s'il est la
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

// --- Tube nomme (Windows) / socket Unix (POSIX) -------------------------------
// Meme interface des deux cotes : le worker ne voit pas la difference.

#ifdef _WIN32
class Pipe {
public:
    ~Pipe() { close(); }

    // `preferred` : emplacement ayant deja fonctionne, essaye en premier.
    // `avoidMask` : emplacements deja ecartes pendant cette tentative.
    //
    // Sans preference, le balayage repart toujours de zero. Sur une machine
    // ou DEUX clients Discord tournent, la reconnexion atterrit alors
    // tantot sur l'un tantot sur l'autre : le journal montrait deux
    // utilisateurs en alternance et une presence qui ne tenait jamais.
    bool open(int preferred = -1, unsigned avoidMask = 0) {
        close();
        for (int n = -1; n < 10; ++n) {
            const int i = (n < 0) ? preferred : n;
            if (i < 0 || i > 9) continue;
            if (n >= 0 && i == preferred) continue; // deja tente au tour -1
            if (avoidMask & (1u << i)) continue;
            wchar_t path[64];
            std::swprintf(path, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                h_ = h;
                slot_ = i;
                return true;
            }
        }
        return false;
    }

    int slot() const { return slot_; }

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
    int slot_ = -1;
};

#else // POSIX : socket Unix, non-bloquante + poll() ( memes delais que Windows)

class Pipe {
public:
    ~Pipe() { close(); }

    // Voir le commentaire de la version Windows : preference pour le
    // dernier emplacement qui a marche, et exclusion de ceux deja ecartes.
    bool open(int preferred = -1, unsigned avoidMask = 0) {
        close();
        // $XDG_RUNTIME_DIR, repli /tmp (jamais le HOME : perms et menage).
        std::string base;
        if (const char* r = std::getenv("XDG_RUNTIME_DIR"); r && *r) base = r;
        else base = "/tmp";
        for (int n = -1; n < 10; ++n) {
            const int i = (n < 0) ? preferred : n;
            if (i < 0 || i > 9) continue;
            if (n >= 0 && i == preferred) continue;
            if (avoidMask & (1u << i)) continue;
            const std::string path = base + "/discord-ipc-" + std::to_string(i);
            const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return false;
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            if (path.size() >= sizeof(addr.sun_path)) {
                ::close(fd);
                continue;
            }
            std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
            if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                // Non-bloquant : toutes les attentes passent par poll().
                const int fl = ::fcntl(fd, F_GETFL, 0);
                if (fl >= 0) ::fcntl(fd, F_SETFL, fl | O_NONBLOCK);
                fd_ = fd;
                slot_ = i;
                return true;
            }
            ::close(fd);
        }
        return false;
    }

    void close() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    bool valid() const { return fd_ >= 0; }

    int slot() const { return slot_; }

    bool write_frame(std::uint32_t opcode, const std::string& payload) {
        if (!valid()) return false;
        std::string buf;
        buf.resize(8 + payload.size());
        const std::uint32_t len = static_cast<std::uint32_t>(payload.size());
        // Entiers 32 bits little-endian (x86/x64 : copie directe).
        std::memcpy(buf.data(), &opcode, 4);
        std::memcpy(buf.data() + 4, &len, 4);
        std::memcpy(buf.data() + 8, payload.data(), payload.size());

        size_t done = 0;
        while (done < buf.size()) {
            const ssize_t n = ::send(fd_, buf.data() + done, buf.size() - done,
                                     MSG_NOSIGNAL); // pas de SIGPIPE si Discord meurt
            if (n > 0) {
                done += static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                if (!wait_for(POLLOUT, 2000)) return false;
                continue;
            }
            return false;
        }
        return true;
    }

    // Lit une trame si disponible avant expiration. false = rien / lien mort.
    bool read_frame(std::uint32_t* opcode, std::string* payload, int timeoutMs) {
        if (!valid()) return false;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        char head[8];
        if (!read_exact(head, 8, deadline)) return false;
        std::uint32_t op = 0, len = 0;
        std::memcpy(&op, head, 4);
        std::memcpy(&len, head + 4, 4);
        // Garde-fou : une trame Discord depasse rarement quelques Ko.
        if (len > 1u << 20) return false;
        std::string body(len, '\0');
        if (len && !read_exact(body.data(), len, deadline)) return false;
        if (opcode) *opcode = op;
        if (payload) *payload = std::move(body);
        return true;
    }

private:
    int fd_ = -1;
    int slot_ = -1;

    bool wait_for(short events, int timeoutMs) {
        pollfd p{};
        p.fd = fd_;
        p.events = events;
        const int r = ::poll(&p, 1, timeoutMs);
        return r > 0 && (p.revents & (POLLIN | POLLOUT | POLLHUP));
    }

    // Lit exactement n octets avant l'echeance (socket non-bloquante).
    bool read_exact(char* dst, std::uint32_t n, std::chrono::steady_clock::time_point deadline) {
        std::uint32_t got = 0;
        while (got < n) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() < 0 || !wait_for(POLLIN, static_cast<int>(left.count())))
                return false;
            const ssize_t r = ::recv(fd_, dst + got, n - got, 0);
            if (r > 0) {
                got += static_cast<std::uint32_t>(r);
                continue;
            }
            if (r == 0) return false; // pair ferme : lien mort
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }
        return true;
    }
};

#endif

std::string nonce() {
    static std::atomic<unsigned> n{0};
    return std::to_string(now_unix()) + "-" + std::to_string(++n);
}

// PID du launcher pour le champ « pid » des trames SET_ACTIVITY.
int self_pid() {
#ifdef _WIN32
    return static_cast<int>(::GetCurrentProcessId());
#else
    return static_cast<int>(::getpid());
#endif
}

// Comparaison insensible a la casse : _stricmp (Win32) / strcasecmp (POSIX).
int strCaseCmp(const char* a, const char* b) {
#ifdef _WIN32
    return ::_stricmp(a, b);
#else
    return ::strcasecmp(a, b);
#endif
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

        // Le chargeur et la version vont dans l'infobulle de la GRANDE
        // image. Ils occupaient avant une petite vignette a laquelle on
        // donnait « logo », c'est-a-dire la MEME image que la grande : le
        // logo se superposait a lui-meme en badge dans un coin, ce qui
        // passait pour un defaut d'affichage. Discord n'affiche `small_text`
        // que s'il y a une `small_image`, donc retirer l'une impose de
        // deplacer l'autre.
        const long long totalHours = d.instPlaySeconds / 3600;
        std::string tip = d.loader + " • Minecraft " + d.mcVersion;
        if (totalHours > 0) tip += " • " + std::to_string(totalHours) + " h";
        assets["large_text"] = tip;
    } else {
        activity["details"] = "Dans le launcher";
        activity["state"] =
            "Temps de jeu total : " + format_hours(d.totalPlaySeconds);
    }

    activity["assets"] = assets;
    // PAS de `buttons` : Discord a retire les boutons de la Rich Presence
    // via IPC, et un payload avec `buttons` fait rejeter toute la mise a
    // jour (plus de presence du tout). Le site reste dans large_text.
    return activity;
}

// Traduit la charge SET_ACTIVITY (JSON du canal direct) vers les champs
// attendus par le SDK. Les deux chemins partent ainsi du MEME
// build_activity : une presence identique, quel que soit le transport.
social::PresenceFields to_fields(const json& a) {
    social::PresenceFields f;
    f.details = a.value("details", std::string{});
    f.state = a.value("state", std::string{});
    if (auto t = a.find("timestamps"); t != a.end() && t->is_object())
        f.startUnix = t->value("start", 0LL);
    if (auto s = a.find("assets"); s != a.end() && s->is_object()) {
        f.largeImage = s->value("large_image", std::string{});
        f.largeText = s->value("large_text", std::string{});
        f.smallImage = s->value("small_image", std::string{});
        f.smallText = s->value("small_text", std::string{});
    }
    return f;
}

void worker() {
    Pipe pipe;
    bool handshaked = false;
    int lastGood = -1; // emplacement IPC qui a repondu la derniere fois
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

        // --- Le SDK social a la priorite -------------------------------
        // S'il est connecte, c'est lui qui porte la presence. Garder en
        // plus notre canal IPC direct ouvert avec le MEME client_id fait
        // que Discord ferme l'une des deux connexions a chaque fois : le
        // journal montrait un battement toutes les trente secondes, et la
        // presence n'avait jamais le temps de s'afficher.
        if (social::presence_active()) {
            if (handshaked) {
                plog("SDK social connecté : la présence passe par lui, "
                     "fermeture du canal direct.");
                pipe.close();
                handshaked = false;
                std::lock_guard<std::mutex> lk(ctx().m);
                ctx().connected = false;
            }
            if (dirty) social::update_presence(to_fields(build_activity(want)));
            continue;
        }

        // --- Connexion / reconnexion (chien de garde 20 s) ---
        if (!handshaked) {
            if (std::chrono::steady_clock::now() - lastAttempt < kRetryDelay) continue;
            lastAttempt = std::chrono::steady_clock::now();
            const std::string appId = DataStore::settings.discordAppId;
            if (appId.empty()) continue;

            // On essaie les emplacements l'un apres l'autre DANS LA MEME
            // tentative, au lieu de buter vingt secondes sur le premier.
            // Observe dans le journal : « pas de réponse à la poignée de
            // main » toutes les 21 s pendant plus d'une minute, parce
            // qu'un emplacement acceptait la connexion sans jamais
            // repondre, et qu'on y revenait sans cesse.
            std::string body;
            unsigned avoid = 0;
            for (int tries = 0; tries < 10 && !handshaked; ++tries) {
                if (!pipe.open(lastGood, avoid)) {
                    if (tries == 0)
                        plog("connexion échouée (Discord est-il ouvert ?).");
                    break;
                }
                const json hs = {{"v", 1}, {"client_id", appId}};
                std::uint32_t op = 0;
                body.clear();
                // op == 2 (CLOSE) en reponse = refus, typiquement un
                // client_id que Discord ne reconnait pas. L'ancien code
                // ne regardait pas l'opcode et se croyait connecte.
                if (pipe.write_frame(0, hs.dump()) &&
                    pipe.read_frame(&op, &body, 3000) && op != 2) {
                    handshaked = true;
                    lastGood = pipe.slot();
                    break;
                }
                plog("emplacement " + std::to_string(pipe.slot()) +
                     " muet ou refusé, essai du suivant.");
                avoid |= 1u << pipe.slot();
                lastGood = -1;
                pipe.close();
            }
            if (!handshaked) continue;
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

        // Draine ce que Discord a envoye. A CHAQUE tour, et apres chaque
        // envoi — pas seulement quand il n'y a rien a pousser. Les accuses
        // de reception de SET_ACTIVITY n'etaient jamais lus : ils
        // s'accumulaient dans le tube, et un CLOSE poli se retrouvait
        // derriere cette file, donc vu avec plusieurs secondes de retard.
        auto drain = [&](int timeoutMs) {
            for (int i = 0; i < 16 && handshaked; ++i) {
                std::uint32_t op = 0;
                std::string in;
                if (!pipe.read_frame(&op, &in, timeoutMs)) return;
                timeoutMs = 0; // les suivantes sans attendre
                if (op == 2) {  // CLOSE
                    plog("connexion fermée par Discord (le chien de garde "
                         "relance).");
                    pipe.close();
                    handshaked = false;
                    std::lock_guard<std::mutex> lk(ctx().m);
                    ctx().connected = false;
                    return;
                }
            }
        };

        if (!dirty) {
            drain(0);
            continue;
        }

        const json frame = {{"cmd", "SET_ACTIVITY"},
                            {"nonce", nonce()},
                            {"args",
                             {{"pid", self_pid()},
                              {"activity", build_activity(want)}}}};
        if (!pipe.write_frame(1, frame.dump())) {
            plog("envoi impossible, lien perdu.");
            pipe.close();
            handshaked = false;
            std::lock_guard<std::mutex> lk(ctx().m);
            ctx().connected = false;
        } else {
            // Lire l'accuse tout de suite : sinon le tube se remplit, et un
            // refus de Discord (charge invalide, cadence trop rapide) passe
            // inapercu — la presence semble envoyee et n'apparait jamais.
            drain(200);
        }
    }

    // Efface la presence avant de partir (C# ClearPresence).
    if (handshaked) {
        const json clear = {{"cmd", "SET_ACTIVITY"},
                            {"nonce", nonce()},
                            {"args",
                             {{"pid", self_pid()},
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
    // Le SDK social compte comme connecte : c'est lui qui porte la
    // presence quand il est la, et l'interface n'a pas a savoir par quel
    // transport elle passe.
    if (social::presence_active()) return true;
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
                if (strCaseCmp(h.c_str(), host.c_str()) == 0) {
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

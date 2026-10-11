#include "plugin_host_client.hpp"

#include "game_launcher.hpp" // log_line
#include "pipe_child.hpp"
#include "plugin_proto.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <climits>
#include <unistd.h>
#endif

using nlohmann::json;
namespace fs = std::filesystem;
namespace pp = tl::pluginproto;

namespace tl::pluginhost {

namespace {

struct Ctx {
    std::mutex m;
    std::condition_variable cv;
    std::unique_ptr<pipechild::Child> host;
    std::thread reader;
    std::mutex wm; // écriture vers le host
    bool ready = false;
    bool alive = false;
    // Ce que le LAUNCHER a accordé, plugin par plugin. Source de vérité
    // pour revalider les appels — jamais ce que le host affirme.
    std::map<std::string, std::set<std::string>> granted;
    std::deque<Call> calls;
    std::deque<Event> events;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

void send(const json& j) {
    auto& c = ctx();
    const std::string f =
        pp::encode_frame(j.dump(-1, ' ', false, json::error_handler_t::replace));
    std::lock_guard<std::mutex> lk(c.wm);
    if (c.host) c.host->write_all(f.data(), f.size());
}

void push_event(Event e) {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    // File bornée : un host bavard ne doit pas faire gonfler la mémoire du
    // launcher si l'interface ne pompe plus (fenêtre minimisée longtemps).
    if (c.events.size() > 512) c.events.pop_front();
    c.events.push_back(std::move(e));
}

// Revalidation d'un appel reçu du host. Voir l'en-tête : le host n'est pas
// cru sur parole.
bool revalidate(const json& j, Call* out, std::string* why) {
    const std::string plugin = j.value("plugin", std::string{});
    if (!j.contains("rid") || !j["rid"].is_number_integer()) {
        *why = "rid absent";
        return false;
    }
    const auto rid = j["rid"].get<std::int64_t>();
    if (rid < 0 || rid > 0x7FFFFFFF) {
        *why = "rid hors limites";
        return false;
    }
    // Reconstituer une requête et la faire passer par le MÊME validateur
    // que le host : méthode dans la table, paramètres conformes.
    const json req{{"v", pp::kApiVersion},
                   {"id", rid},
                   {"method", j.value("method", json())},
                   {"params", j.value("params", json::object())}};
    pp::Error err;
    const auto r = pp::parse_request(req.dump(-1, ' ', false, json::error_handler_t::replace),
                                     &err);
    if (!r) {
        *why = "appel invalide : " + err.message;
        return false;
    }
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    const auto g = c.granted.find(plugin);
    if (g == c.granted.end()) {
        *why = "plugin inconnu du launcher : " + plugin;
        return false;
    }
    const std::string perm = *pp::required_permission(r->method);
    if (!perm.empty() && !g->second.count(perm)) {
        *why = "permission non accordée par le launcher : " + perm;
        return false;
    }
    *out = Call{plugin, rid, r->method, r->params};
    return true;
}

void reader_loop() {
    auto& c = ctx();
    pp::FrameReader fr;
    std::vector<char> buf(16 * 1024);
    for (;;) {
        const long n = c.host->read_some(buf.data(), buf.size());
        if (n <= 0) break;
        fr.feed(buf.data(), static_cast<std::size_t>(n));
        std::string frame;
        for (;;) {
            const auto st = fr.next(&frame);
            if (st == pp::FrameReader::Status::NeedMore) break;
            if (st == pp::FrameReader::Status::Invalid) {
                log_line("[Plugins] lien host corrompu : arrêt du host.");
                c.host->terminate();
                goto done;
            }
            const json j = json::parse(frame, nullptr, false);
            if (!j.is_object() || !j.contains("op") || !j["op"].is_string()) continue;
            const std::string op = j["op"].get<std::string>();
            if (op == "ready") {
                std::lock_guard<std::mutex> lk(c.m);
                c.ready = true;
                c.cv.notify_all();
            } else if (op == "call") {
                Call call;
                std::string why;
                if (revalidate(j, &call, &why)) {
                    std::lock_guard<std::mutex> lk(c.m);
                    if (c.calls.size() < 256) {
                        c.calls.push_back(std::move(call));
                        continue;
                    }
                    why = "file d'appels pleine";
                }
                // Refusé ici : le host est-il défectueux, ou compromis ?
                // Dans les deux cas, on répond par une erreur ET on le note.
                log_line("[Plugins] appel rejeté par le launcher : " + why);
                if (j.contains("rid") && j["rid"].is_number_integer())
                    send({{"op", "reply"},
                          {"rid", j["rid"]},
                          {"error", {{"code", pp::kPermissionDenied}, {"message", "refusé"}}}});
            } else if (op == "event") {
                push_event(Event{j.value("plugin", std::string{}),
                                 j.value("kind", std::string{}),
                                 j.value("detail", std::string{})});
            } else if (op == "log") {
                const std::string plugin = j.value("plugin", std::string{});
                const std::string msg = j.value("message", std::string{});
                log_line("[Plugin " + plugin + "] " + msg);
                push_event(Event{plugin, "log", j.value("level", std::string{}) + ": " + msg});
            }
        }
    }
done:
    {
        std::lock_guard<std::mutex> lk(c.m);
        c.alive = false;
        c.ready = false;
        c.granted.clear();
        c.cv.notify_all();
    }
    push_event(Event{"", "host_died", ""});
}

} // namespace

fs::path default_host_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    fs::path self = n ? fs::path(std::wstring(buf, n)) : fs::path();
    return self.parent_path() / "tl_plugin_host.exe";
#else
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    fs::path self = n > 0 ? fs::path(std::string(buf, static_cast<std::size_t>(n))) : fs::path();
    return self.parent_path() / "tl_plugin_host";
#endif
}

bool start(const fs::path& hostExe, std::string* err) {
    auto& c = ctx();
    if (running()) return true;
    std::error_code ec;
    if (!fs::is_regular_file(hostExe, ec)) {
        if (err) *err = "hôte de plugins introuvable : " + hostExe.string();
        return false;
    }
    auto child = std::make_unique<pipechild::Child>();
    pipechild::Options o;
    o.program = hostExe.string();
    if (!child->start(o, err)) return false;
    // Ancien fil lecteur (host précédent mort) : attendu HORS verrou, car
    // il prend lui-même c.m pour se terminer.
    if (c.reader.joinable()) c.reader.join();
    {
        std::lock_guard<std::mutex> lk(c.m);
        c.host = std::move(child);
        c.alive = true;
        c.ready = false;
    }
    c.reader = std::thread(reader_loop);
    std::unique_lock<std::mutex> lk(c.m);
    if (!c.cv.wait_for(lk, std::chrono::seconds(5), [&] { return c.ready || !c.alive; }) ||
        !c.ready) {
        lk.unlock();
        if (err) *err = "l'hôte de plugins n'a pas répondu";
        stop();
        return false;
    }
    return true;
}

void stop() {
    auto& c = ctx();
    std::unique_ptr<pipechild::Child> h;
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (!c.host) return;
    }
    send({{"op", "shutdown"}});
    {
        std::lock_guard<std::mutex> lk(c.wm);
        c.host->close_stdin();
    }
    // Laisse au host le temps d'arrêter proprement ses plugins, puis tue.
    for (int i = 0; i < 40 && c.host->alive(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    c.host->terminate();
    if (c.reader.joinable()) c.reader.join();
    std::lock_guard<std::mutex> lk(c.m);
    std::lock_guard<std::mutex> lk2(c.wm);
    c.host.reset();
    c.calls.clear();
    c.granted.clear();
}

bool running() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    return c.host && c.alive && c.ready;
}

bool start_plugin(const Launch& l, std::string* err) {
    if (!running()) {
        if (err) *err = "hôte de plugins arrêté";
        return false;
    }
    std::set<std::string> perms;
    json permsJ = json::array();
    for (const auto& p : l.permissions)
        if (pp::is_known_permission(p)) {
            perms.insert(p);
            permsJ.push_back(p);
        }
    {
        auto& c = ctx();
        std::lock_guard<std::mutex> lk(c.m);
        c.granted[l.id] = perms;
    }
    send({{"op", "start"},
          {"plugin",
           {{"id", l.id},
            {"program", l.program},
            {"args", l.args},
            {"workDir", l.workDir},
            {"permissions", permsJ},
            {"stderrPath", l.stderrPath}}}});
    return true;
}

void stop_plugin(const std::string& id) {
    {
        auto& c = ctx();
        std::lock_guard<std::mutex> lk(c.m);
        c.granted.erase(id); // d'abord : plus aucun appel accepté
    }
    send({{"op", "stop"}, {"id", id}});
}

void kill_all() {
    {
        auto& c = ctx();
        std::lock_guard<std::mutex> lk(c.m);
        c.granted.clear(); // d'abord : même un appel déjà en route est refusé
        c.calls.clear();
    }
    send({{"op", "killall"}});
}

void pump(const Handler& handler, const EventSink& onEvent) {
    auto& c = ctx();
    std::deque<Call> calls;
    std::deque<Event> events;
    {
        std::lock_guard<std::mutex> lk(c.m);
        calls.swap(c.calls);
        events.swap(c.events);
    }
    for (const auto& call : calls) {
        // Revérifié au moment de traiter : un kill_all ou un stop_plugin
        // survenu entre la mise en file et maintenant l'emporte.
        bool still = false;
        {
            std::lock_guard<std::mutex> lk(c.m);
            still = c.granted.count(call.plugin) != 0;
        }
        if (!still) {
            send({{"op", "reply"},
                  {"rid", call.rid},
                  {"error", {{"code", pp::kPluginDisabled}, {"message", "plugin arrêté"}}}});
            continue;
        }
        int code = 0;
        std::string msg;
        json result;
        try {
            result = handler ? handler(call, &code, &msg) : json::object();
        } catch (const std::exception&) {
            // Jamais le détail d'une exception vers du code tiers.
            code = pp::kInternal;
            msg = "erreur interne";
        }
        if (code != 0)
            send({{"op", "reply"}, {"rid", call.rid}, {"error", {{"code", code}, {"message", msg}}}});
        else
            send({{"op", "reply"}, {"rid", call.rid}, {"result", result}});
    }
    if (onEvent)
        for (const auto& e : events) onEvent(e);
}

} // namespace tl::pluginhost

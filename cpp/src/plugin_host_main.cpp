// tl_plugin_host — V8.
//
// Processus intermédiaire entre le launcher et les plugins. Voir
// plugin_proto.hpp pour la frontière de confiance ; en deux mots :
//
//     launcher  <──(stdin/stdout)──>  tl_plugin_host  <──(tubes)──>  plugins
//
// Ce programme LIT des octets écrits par du code tiers. C'est pour cela
// qu'il existe : s'il est compromis, l'attaquant obtient un processus qui
// ne détient ni jeton Microsoft, ni clé, ni secret, et dont la seule
// sortie vers le launcher est un canal où ne passent que des appels déjà
// validés. Il ne lie PAS tl_core.
//
// Ce que le host garantit au launcher :
//   - toute requête transmise a passé plugin_proto::parse_request ;
//   - la méthode est couverte par les permissions accordées au plugin ;
//   - le débit est borné (seau à jetons, appels en vol plafonnés) ;
//   - un plugin qui abuse est TUÉ, pas seulement freiné.

#include "pipe_child.hpp"
#include "plugin_proto.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <csignal>
#include <sys/prctl.h>
#include <unistd.h>
#endif

using nlohmann::json;
namespace pp = tl::pluginproto;
namespace pc = tl::pipechild;
using Clock = std::chrono::steady_clock;

namespace {

// --- Limites --------------------------------------------------------------
// Volontairement serrées : l'API v1 n'a rien qui justifie un débit élevé.
constexpr double kRatePerSec = 20.0;  // jetons regagnés par seconde
constexpr double kBurst = 40.0;       // réserve maximale
constexpr int kMaxInflight = 16;      // appels transmis sans réponse
constexpr int kKillAfterDenials = 20; // sonder l'API n'est pas un bug
constexpr int kKillAfterRateHits = 200;
constexpr int kKillAfterBadRequests = 20;
constexpr std::size_t kMaxPlugins = 32;

// --- Durcissement du processus lui-même -----------------------------------

void harden_self() {
#ifdef _WIN32
    // Pas de code généré à l'exécution : le host n'a pas de JIT, et cela
    // ferme la porte à l'injection de shellcode par allocation exécutable.
    PROCESS_MITIGATION_DYNAMIC_CODE_POLICY dyn{};
    dyn.ProhibitDynamicCode = 1;
    SetProcessMitigationPolicy(ProcessDynamicCodePolicy, &dyn, sizeof(dyn));
    // Pas de DLL injectée par les anciens points d'extension (AppInit,
    // hooks de fenêtres…).
    PROCESS_MITIGATION_EXTENSION_POINT_DISABLE_POLICY ext{};
    ext.DisableExtensionPoints = 1;
    SetProcessMitigationPolicy(ProcessExtensionPointDisablePolicy, &ext, sizeof(ext));
    // Pas d'image chargée depuis un partage réseau ou un fichier à faible
    // intégrité — deux vecteurs classiques de détournement de DLL.
    PROCESS_MITIGATION_IMAGE_LOAD_POLICY img{};
    img.NoRemoteImages = 1;
    img.NoLowMandatoryLabelImages = 1;
    img.PreferSystem32Images = 1;
    SetProcessMitigationPolicy(ProcessImageLoadPolicy, &img, sizeof(img));
    // Un handle invalide utilisé est une faute, pas une erreur ignorée.
    PROCESS_MITIGATION_STRICT_HANDLE_CHECK_POLICY sh{};
    sh.RaiseExceptionOnInvalidHandleReference = 1;
    sh.HandleExceptionsPermanentlyEnabled = 1;
    SetProcessMitigationPolicy(ProcessStrictHandleCheckPolicy, &sh, sizeof(sh));
#else
    // Aucun exécutable lancé par nous ne pourra gagner de privilèges
    // (setuid, capacités de fichier).
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    // Pas de vidage mémoire, pas d'attachement ptrace par un autre
    // processus de l'utilisateur.
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    // Mourir avec le launcher. Si celui-ci est déjà mort avant cet appel,
    // notre parent est devenu init : on part tout de suite.
    const pid_t parent = getppid();
    prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0);
    if (getppid() != parent) std::_Exit(0);
#endif
}

// --- État -----------------------------------------------------------------

struct Plugin {
    std::string id;
    std::set<std::string> perms;
    pc::Child child;
    std::mutex wm; // écriture vers le plugin
    std::thread reader;
    std::atomic<bool> dead{false};
    // Compteurs, touchés seulement par le fil lecteur du plugin.
    bool helloed = false;
    double tokens = kBurst;
    Clock::time_point last = Clock::now();
    int denials = 0, rateHits = 0, badRequests = 0;
    std::atomic<int> inflight{0};
};

struct Pending {
    std::string plugin;
    std::int64_t reqId = 0; // id choisi par le PLUGIN, rendu tel quel
};

std::mutex g_m; // g_plugins, g_pending, g_nextRid
std::map<std::string, std::unique_ptr<Plugin>> g_plugins;
std::map<std::int64_t, Pending> g_pending;
std::int64_t g_nextRid = 1;
std::mutex g_out; // écriture vers le launcher

void to_launcher(const json& j) {
    const std::string f =
        pp::encode_frame(j.dump(-1, ' ', false, json::error_handler_t::replace));
    std::lock_guard<std::mutex> lk(g_out);
    pc::stdio_write(f.data(), f.size());
}

void event(const std::string& plugin, const char* kind, const std::string& detail = {}) {
    to_launcher({{"op", "event"}, {"plugin", plugin}, {"kind", kind}, {"detail", detail}});
}

void to_plugin(Plugin& p, const std::string& payload) {
    const std::string f = pp::encode_frame(payload);
    std::lock_guard<std::mutex> lk(p.wm);
    if (!p.child.write_all(f.data(), f.size())) p.dead = true;
}

void kill_plugin(Plugin& p, const std::string& why) {
    if (p.dead.exchange(true)) return;
    p.child.terminate();
    event(p.id, "killed", why);
}

// Identifiant de plugin : nom de dossier, contrôlé par le launcher, mais
// revérifié ici — il sert de clé et apparaît dans les journaux.
bool valid_plugin_id(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.'))
            return false;
    return s[0] != '.';
}

// --- Requêtes d'un plugin --------------------------------------------------

bool take_token(Plugin& p) {
    const auto now = Clock::now();
    const double dt = std::chrono::duration<double>(now - p.last).count();
    p.last = now;
    p.tokens = std::min(kBurst, p.tokens + dt * kRatePerSec);
    if (p.tokens < 1.0) return false;
    p.tokens -= 1.0;
    return true;
}

void handle_frame(Plugin& p, const std::string& frame) {
    pp::Error err;
    const auto req = pp::parse_request(frame, &err);
    if (!req) {
        to_plugin(p, pp::make_error(err));
        if (++p.badRequests > kKillAfterBadRequests)
            kill_plugin(p, "requêtes invalides répétées");
        return;
    }

    if (req->method == "host.hello") {
        const auto want = req->params["api"].get<std::int64_t>();
        if (want > pp::kApiVersion) {
            to_plugin(p, pp::make_error({pp::kInvalidRequest,
                                         "version d'API non prise en charge par ce launcher",
                                         req->id}));
            return;
        }
        p.helloed = true;
        json perms = json::array();
        for (const auto& s : p.perms) perms.push_back(s);
        to_plugin(p, pp::make_result(req->id, {{"api", pp::kApiVersion},
                                               {"plugin", p.id},
                                               {"permissions", perms}}));
        return;
    }
    if (!p.helloed) {
        to_plugin(p, pp::make_error({pp::kInvalidRequest, "host.hello attendu d'abord", req->id}));
        return;
    }
    if (!take_token(p)) {
        to_plugin(p, pp::make_error({pp::kRateLimited, "trop de requêtes", req->id}));
        if (++p.rateHits > kKillAfterRateHits) kill_plugin(p, "débit abusif");
        return;
    }

    if (req->method == "log.write") {
        to_launcher({{"op", "log"},
                     {"plugin", p.id},
                     {"level", req->params["level"]},
                     {"message", req->params["message"]}});
        to_plugin(p, pp::make_result(req->id, json::object()));
        return;
    }

    const std::string perm = *pp::required_permission(req->method);
    if (!perm.empty() && !p.perms.count(perm)) {
        to_plugin(p, pp::make_error({pp::kPermissionDenied,
                                     "permission non accordée : " + perm, req->id}));
        event(p.id, "denied", req->method);
        if (++p.denials > kKillAfterDenials) kill_plugin(p, "permissions sondées à répétition");
        return;
    }
    if (p.inflight.load() >= kMaxInflight) {
        to_plugin(p, pp::make_error({pp::kRateLimited, "trop d'appels en attente", req->id}));
        return;
    }

    // Identifiant attribué PAR LE HOST : on ne route jamais une réponse
    // d'après un id choisi par le plugin, qui pourrait viser l'appel d'un
    // autre plugin.
    std::int64_t rid;
    {
        std::lock_guard<std::mutex> lk(g_m);
        rid = g_nextRid++;
        g_pending[rid] = Pending{p.id, req->id};
    }
    ++p.inflight;
    to_launcher({{"op", "call"},
                 {"plugin", p.id},
                 {"rid", rid},
                 {"method", req->method},
                 {"params", req->params}});
}

void reader_loop(Plugin* p) {
    pp::FrameReader fr;
    std::vector<char> buf(16 * 1024);
    for (;;) {
        const long n = p->child.read_some(buf.data(), buf.size());
        if (n <= 0) break;
        fr.feed(buf.data(), static_cast<std::size_t>(n));
        std::string frame;
        for (;;) {
            const auto st = fr.next(&frame);
            if (st == pp::FrameReader::Status::NeedMore) break;
            if (st == pp::FrameReader::Status::Invalid) {
                // Longueur mensongère : on ne sait plus lire ce flux.
                kill_plugin(*p, "trame invalide");
                return;
            }
            handle_frame(*p, frame);
            if (p->dead) return;
        }
    }
    if (!p->dead.exchange(true)) event(p->id, "exited");
}

// --- Ordres du launcher ----------------------------------------------------

// Arrêter un plugin se fait en DEUX temps, et l'ordre compte.
//
// 1. Sous g_m : on le retire de la table et on oublie ses appels en vol.
// 2. HORS de g_m : on le tue et on attend la fin de son fil lecteur.
//
// Attendre le fil en tenant g_m serait un interblocage : ce fil prend
// lui-même g_m pour transmettre un appel au launcher. Un plugin qui
// envoyait une requête au moment précis où on l'arrêtait aurait figé le
// host — défaut repéré à la relecture, avant tout test.
std::unique_ptr<Plugin> detach_locked(const std::string& id) {
    auto it = g_plugins.find(id);
    if (it == g_plugins.end()) return nullptr;
    std::unique_ptr<Plugin> p = std::move(it->second);
    g_plugins.erase(it);
    for (auto pit = g_pending.begin(); pit != g_pending.end();)
        pit = pit->second.plugin == id ? g_pending.erase(pit) : std::next(pit);
    return p;
}

void finish(std::unique_ptr<Plugin> p) {
    if (!p) return;
    p->dead = true;
    p->child.terminate();
    if (p->reader.joinable()) p->reader.join();
}

void op_start(const json& j) {
    const auto it = j.find("plugin");
    if (it == j.end() || !it->is_object()) return;
    const json& d = *it;
    const std::string id = d.value("id", std::string{});
    if (!valid_plugin_id(id)) {
        event(id, "failed", "identifiant de plugin invalide");
        return;
    }
    auto p = std::make_unique<Plugin>();
    p->id = id;
    if (const auto ps = d.find("permissions"); ps != d.end() && ps->is_array())
        for (const auto& s : *ps)
            if (s.is_string() && pp::is_known_permission(s.get<std::string>()))
                p->perms.insert(s.get<std::string>());

    pc::Options o;
    o.program = d.value("program", std::string{});
    o.workDir = d.value("workDir", std::string{});
    o.stderrPath = d.value("stderrPath", std::string{});
    if (const auto as = d.find("args"); as != d.end() && as->is_array())
        for (const auto& a : *as)
            if (a.is_string()) o.args.push_back(a.get<std::string>());

    // Relance propre d'un plugin déjà connu (voir detach_locked).
    std::unique_ptr<Plugin> old;
    std::size_t count = 0;
    {
        std::lock_guard<std::mutex> lk(g_m);
        old = detach_locked(id);
        count = g_plugins.size();
    }
    finish(std::move(old));
    if (count >= kMaxPlugins) {
        event(id, "failed", "trop de plugins actifs");
        return;
    }
    std::string err;
    if (!p->child.start(o, &err)) {
        event(id, "failed", err);
        return;
    }
    Plugin* raw = p.get();
    raw->reader = std::thread(reader_loop, raw);
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_plugins[id] = std::move(p);
    }
    event(id, "started", std::to_string(raw->child.pid()));
}

void op_reply(const json& j) {
    if (!j.contains("rid") || !j["rid"].is_number_integer()) return;
    const auto rid = j["rid"].get<std::int64_t>();
    Plugin* p = nullptr;
    Pending pend;
    {
        std::lock_guard<std::mutex> lk(g_m);
        auto it = g_pending.find(rid);
        if (it == g_pending.end()) return; // plugin arrêté entre-temps
        pend = it->second;
        g_pending.erase(it);
        auto pit = g_plugins.find(pend.plugin);
        if (pit == g_plugins.end()) return;
        p = pit->second.get();
    }
    // Seul le fil principal retire des plugins : `p` reste valide ici.
    --p->inflight;
    if (const auto e = j.find("error"); e != j.end() && e->is_object()) {
        pp::Error er;
        er.id = pend.reqId;
        er.code = e->value("code", static_cast<int>(pp::kInternal));
        er.message = e->value("message", std::string("erreur"));
        to_plugin(*p, pp::make_error(er));
    } else {
        to_plugin(*p, pp::make_result(pend.reqId, j.value("result", json::object())));
    }
}

void stop_one(const std::string& id) {
    std::unique_ptr<Plugin> p;
    {
        std::lock_guard<std::mutex> lk(g_m);
        p = detach_locked(id);
    }
    finish(std::move(p));
}

void stop_all() {
    std::vector<std::unique_ptr<Plugin>> all;
    {
        std::lock_guard<std::mutex> lk(g_m);
        std::vector<std::string> ids;
        for (const auto& [id, _] : g_plugins) ids.push_back(id);
        for (const auto& id : ids) all.push_back(detach_locked(id));
    }
    for (auto& p : all) finish(std::move(p));
}

} // namespace

int main() {
    harden_self();
    to_launcher({{"op", "ready"}, {"api", pp::kApiVersion}});

    pp::FrameReader fr;
    std::vector<char> buf(16 * 1024);
    for (;;) {
        const long n = pc::stdio_read(buf.data(), buf.size());
        if (n <= 0) break; // le launcher est parti
        fr.feed(buf.data(), static_cast<std::size_t>(n));
        std::string frame;
        bool quit = false;
        for (;;) {
            const auto st = fr.next(&frame);
            if (st == pp::FrameReader::Status::NeedMore) break;
            if (st == pp::FrameReader::Status::Invalid) {
                quit = true; // lien launcher corrompu : on ne devine pas
                break;
            }
            const json j = json::parse(frame, nullptr, false);
            if (!j.is_object() || !j.contains("op") || !j["op"].is_string()) continue;
            const std::string op = j["op"].get<std::string>();
            if (op == "start") op_start(j);
            else if (op == "reply") op_reply(j);
            else if (op == "stop") {
                stop_one(j.value("id", std::string{}));
            } else if (op == "killall") {
                stop_all();
                event("", "killall");
            } else if (op == "shutdown") {
                quit = true;
                break;
            }
        }
        if (quit) break;
    }
    stop_all();
    return 0;
}

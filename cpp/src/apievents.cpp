#include "apievents.hpp"

#include "datastore.hpp"
#include "http_win.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace tl::events {

const char* const kCrash = "crash";
const char* const kGameStart = "game_start";
const char* const kGameStop = "game_stop";

namespace {

struct Pending {
    std::string hookId;
    std::string url;
    std::string secret;
    std::string body;
};

struct Ctx {
    std::mutex m;
    std::condition_variable cv;
    std::thread th;
    bool started = false;
    bool stopping = false;
    std::deque<Pending> queue;
    std::vector<Hook> hooks;
    bool loaded = false;
    bool dirty = false;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

long long now_unix() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<Hook> parse(const nlohmann::json& j) {
    std::vector<Hook> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        Hook h;
        h.id = e.value("id", std::string{});
        h.url = e.value("url", std::string{});
        h.secret = e.value("secret", std::string{});
        if (auto v = e.find("events"); v != e.end() && v->is_array())
            for (const auto& t : *v)
                if (t.is_string() && is_known_event(t.get<std::string>()))
                    h.events.push_back(t.get<std::string>());
        h.enabled = e.value("enabled", true);
        h.lastUnix = e.value("lastUnix", 0LL);
        h.lastOk = e.value("lastOk", false);
        h.failures = e.value("failures", 0);
        h.lastError = e.value("lastError", std::string{});
        if (h.id.empty() || h.url.empty()) continue;
        out.push_back(std::move(h));
    }
    return out;
}

nlohmann::json dump(const std::vector<Hook>& hooks) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& h : hooks)
        arr.push_back({{"id", h.id},
                       {"url", h.url},
                       {"secret", h.secret},
                       {"events", h.events},
                       {"enabled", h.enabled},
                       {"lastUnix", h.lastUnix},
                       {"lastOk", h.lastOk},
                       {"failures", h.failures},
                       {"lastError", h.lastError}});
    return arr;
}

// Appelé avec le verrou tenu.
std::vector<Hook>& hooks_locked(Ctx& c) {
    if (!c.loaded) {
        c.loaded = true;
        c.hooks = parse(DataStore::settings.eventHooks);
    }
    return c.hooks;
}

void save_locked(Ctx& c) {
    DataStore::settings.eventHooks = dump(c.hooks);
    DataStore::save();
    c.dirty = false;
}

void note_result(const std::string& id, bool ok, const std::string& err) {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    for (auto& h : hooks_locked(c))
        if (h.id == id) {
            h.lastUnix = now_unix();
            h.lastOk = ok;
            h.lastError = ok ? std::string{} : err;
            // Un abonnement qui échoue dix fois d'affilée est mort : on le
            // désactive plutôt que de retenter indéfiniment à chaque crash.
            // L'utilisateur le réactive quand il a corrigé l'adresse.
            h.failures = ok ? 0 : h.failures + 1;
            if (h.failures >= 10) {
                h.enabled = false;
                h.lastError = "Désactivé après 10 échecs : " + h.lastError;
            }
            c.dirty = true;
            return;
        }
}

// Envoi effectif. Pas de nouvelle tentative immédiate : l'événement suivant
// servira de test, et insister sur une URL morte ne fait que retarder la
// file.
bool deliver(const Pending& p, std::string* errOut) {
    std::string headers = "Content-Type: application/json";
    if (!p.secret.empty())
        headers += "\r\nX-TeamLauncher-Secret: " + p.secret;
    auto r = http::post_string(p.url, p.body, "application/json", headers);
    if (!r) {
        if (errOut) *errOut = "Aucune réponse (hôte injoignable ou refusé).";
        return false;
    }
    if (r->status < 200 || r->status >= 300) {
        if (errOut) *errOut = "HTTP " + std::to_string(r->status);
        return false;
    }
    return true;
}

void worker() {
    auto& c = ctx();
    for (;;) {
        Pending p;
        {
            std::unique_lock<std::mutex> lk(c.m);
            c.cv.wait(lk, [&] { return c.stopping || !c.queue.empty(); });
            if (c.stopping && c.queue.empty()) return;
            p = std::move(c.queue.front());
            c.queue.pop_front();
        }
        std::string err;
        const bool ok = deliver(p, &err);
        note_result(p.hookId, ok, err);
    }
}

void ensure_worker_locked(Ctx& c) {
    if (c.started) return;
    c.started = true;
    c.th = std::thread(worker);
}

} // namespace

bool is_known_event(const std::string& t) {
    return t == kCrash || t == kGameStart || t == kGameStop;
}

bool url_acceptable(const std::string& url, std::string* whyNot) {
    const std::string u = lower(url);
    const bool https = u.rfind("https://", 0) == 0;
    const bool http = u.rfind("http://", 0) == 0;
    if (!https && !http) {
        if (whyNot) *whyNot = "L'adresse doit commencer par https://.";
        return false;
    }
    if (https) return true;
    // Exception pour le bouclage : on développe souvent son récepteur en
    // local, et là il n'y a pas de réseau à écouter.
    const size_t start = 7; // longueur de « http:// »
    const size_t end = u.find_first_of("/:", start);
    const std::string host =
        u.substr(start, end == std::string::npos ? std::string::npos
                                                 : end - start);
    if (host == "127.0.0.1" || host == "localhost" || host == "[::1]" ||
        host == "::1")
        return true;
    if (whyNot)
        *whyNot =
            "HTTP en clair refusé : le corps contient le nom de l'instance "
            "et un extrait de journal, et le secret voyage en en-tête. "
            "Utilisez https:// (ou une adresse locale pour vos essais).";
    return false;
}

std::vector<Hook> list() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    return hooks_locked(c);
}

bool add(const std::string& url, const std::string& secret,
         const std::vector<std::string>& eventTypes, std::string* errOut) {
    if (!url_acceptable(url, errOut)) return false;
    Hook h;
    h.id = new_guid().substr(0, 12);
    h.url = url;
    h.secret = secret;
    for (const auto& t : eventTypes)
        if (is_known_event(t) &&
            std::find(h.events.begin(), h.events.end(), t) == h.events.end())
            h.events.push_back(t);
    // Aucun type coché : l'abonnement ne servirait à rien. On prend celui
    // qui a motivé la fonctionnalité.
    if (h.events.empty()) h.events.push_back(kCrash);

    // L'hôte n'est joignable qu'après inscription : le refus par défaut de
    // la couche HTTP reste la règle.
    http::allow_host(url);

    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    hooks_locked(c).push_back(std::move(h));
    save_locked(c);
    return true;
}

bool remove(const std::string& id) {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    auto& v = hooks_locked(c);
    const auto before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const Hook& h) { return h.id == id; }),
            v.end());
    if (v.size() == before) return false;
    save_locked(c);
    return true;
}

void set_enabled(const std::string& id, bool on) {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    for (auto& h : hooks_locked(c))
        if (h.id == id) {
            h.enabled = on;
            if (on) h.failures = 0; // réactiver, c'est repartir de zéro
            save_locked(c);
            return;
        }
}

void emit(const std::string& type, const nlohmann::json& payload) {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    const auto& hooks = hooks_locked(c);
    std::vector<Pending> batch;
    for (const auto& h : hooks) {
        if (!h.enabled) continue;
        if (std::find(h.events.begin(), h.events.end(), type) == h.events.end())
            continue;
        nlohmann::json body = payload;
        body["event"] = type;
        body["time"] = now_unix();
        batch.push_back(Pending{h.id, h.url, h.secret, body.dump()});
    }
    if (batch.empty()) return; // aucun abonné : rien à faire, pas même un fil
    for (auto& p : batch) {
        // Plafond : un jeu qui planterait en boucle ne doit pas faire
        // enfler la file indéfiniment.
        if (c.queue.size() >= 64) break;
        c.queue.push_back(std::move(p));
    }
    ensure_worker_locked(c);
    c.cv.notify_one();
}

bool test(const std::string& id, std::string* errOut) {
    Pending p;
    {
        auto& c = ctx();
        std::lock_guard<std::mutex> lk(c.m);
        for (const auto& h : hooks_locked(c))
            if (h.id == id) {
                p.hookId = h.id;
                p.url = h.url;
                p.secret = h.secret;
            }
    }
    if (p.url.empty()) {
        if (errOut) *errOut = "Abonnement introuvable.";
        return false;
    }
    http::allow_host(p.url);
    nlohmann::json body{{"event", "test"},
                        {"time", now_unix()},
                        {"message", "Essai depuis Team Launcher."}};
    p.body = body.dump();
    std::string err;
    const bool ok = deliver(p, &err);
    note_result(p.hookId, ok, err);
    if (!ok && errOut) *errOut = err;
    return ok;
}

void start() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    // Les hôtes enregistrés doivent être joignables dès le premier
    // événement, sans attendre qu'on rouvre les réglages.
    for (const auto& h : hooks_locked(c)) http::allow_host(h.url);
}

void stop() {
    auto& c = ctx();
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(c.m);
        c.stopping = true;
        th = std::move(c.th);
        c.started = false;
    }
    c.cv.notify_all();
    if (th.joinable()) th.join();
    std::lock_guard<std::mutex> lk(c.m);
    if (c.dirty) save_locked(c);
}

} // namespace tl::events

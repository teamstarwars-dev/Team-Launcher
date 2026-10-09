#include "account.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"
#include "social.hpp" // audio vocal (lobby Discord)

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace tl::account {

namespace {

using json = nlohmann::json;

std::mutex& lock() {
    static std::mutex m;
    return m;
}

struct Ctx {
    bool workerOn = false;
    std::thread worker;
    std::shared_ptr<std::atomic<bool>> cancel;
    // Session
    bool sessionOk = false;
    bool authFailed = false;
    std::string error;
    std::string notice;
    std::string sendError;
    std::string callNotice;
    // Cache
    std::vector<Friend> friendsCache;
    std::vector<std::string> pendIn;
    std::vector<std::string> pendOut;
    std::map<std::string, std::vector<Message>> threads;
    std::map<std::string, int> unread;
    std::set<std::string> historyAsked;
    long long lastEventId = 0;
    long long pendingSeq = 0;
    // Appel (un seul a la fois)
    enum class CallDir { None, Out, In };
    CallDir callDir = CallDir::None;
    bool callActive = false;
    std::uint64_t callLobby = 0;
    std::string apiCallId;
    std::string callPeer;
    std::string callSecret;
    bool callMuted = false;
    bool callDeaf = false;
    bool callDenoise = false;
    long long callSince = 0;
    long long callDeadline = 0;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

long long unix_now() {
    return static_cast<long long>(std::chrono::system_clock::to_time_t(
        std::chrono::system_clock::now()));
}

// Journal de diagnostic (TL_ACCOUNT_DEBUG=1) : le worker tourne en fond,
// sans ca on ne voit rien de ce qu'il fait.
void alog(const std::string& msg) {
    if (std::getenv("TL_ACCOUNT_DEBUG")) log_line("[Account] " + msg);
}

// URL de base sans '/' final. Vide = fonction desactivee.
std::string base() {
    std::string b = DataStore::settings.tlApiUrl;
    while (!b.empty() && (b.back() == '/' || b.back() == ' ')) b.pop_back();
    while (!b.empty() && b.front() == ' ') b.erase(b.begin());
    return b;
}

std::string token() { return DataStore::settings.tlApiToken; }

std::string auth_headers() {
    return "Authorization: Bearer " + token() + "\r\n";
}

std::string make_secret() {
    static std::mt19937_64 rng{std::random_device{}()};
    static const char* hex = "0123456789abcdef";
    std::string s = "tla-";
    for (int i = 0; i < 16; i++) s += hex[rng() & 0xF];
    return s;
}

void reset_call_locked(Ctx& c) {
    c.callDir = Ctx::CallDir::None;
    c.callActive = false;
    c.callLobby = 0;
    c.apiCallId.clear();
    c.callPeer.clear();
    c.callSecret.clear();
    c.callMuted = false;
    c.callDeaf = false;
    c.callDenoise = false;
    c.callSince = 0;
    c.callDeadline = 0;
}

// POST/GET avec authentification. nullopt = echec reseau. Le 401 est
// signale via authFailed (pump() nettoie le jeton sur le thread d'UI).
std::optional<http::Response> api_call(const std::string& method,
                                       const std::string& path,
                                       const std::string& body, bool auth,
                                       std::atomic<bool>* cancel = nullptr) {
    const std::string b = base();
    if (b.empty()) return std::nullopt;
    const std::string url = b + path;
    const std::string headers = auth ? auth_headers() : std::string{};
    std::optional<http::Response> r;
    if (method == "POST")
        r = http::post_string(url, body, "application/json", headers, cancel);
    else if (method == "DELETE")
        r = http::post_string(url, body, "application/json",
                              headers + "X-HTTP-Method-Override: DELETE\r\n",
                              cancel);
    else
        r = http::get_response(url, headers, cancel);
    if (r && r->status == 401 && auth) {
        std::lock_guard<std::mutex> lk(lock());
        ctx().authFailed = true;
    }
    return r;
}

// Reponse -> JSON. false = erreur renseignee (message serveur si present).
bool parse_api(const http::Response& r, json& out, std::string* errOut) {
    if (r.status < 200 || r.status >= 300) {
        if (errOut) {
            *errOut = "Erreur " + std::to_string(r.status);
            try {
                const json j = json::parse(r.body);
                if (j.contains("error") && j["error"].is_string())
                    *errOut = j["error"].get<std::string>();
            } catch (const std::exception&) {
            }
        }
        return false;
    }
    try {
        out = json::parse(r.body);
    } catch (const std::exception&) {
        if (errOut) *errOut = "Réponse incompréhensible.";
        return false;
    }
    return true;
}

bool need_auth(std::string* errOut) {
    if (base().empty()) {
        if (errOut)
            *errOut = "Compte Team Launcher non configuré (URL d'API).";
        return false;
    }
    if (token().empty()) {
        if (errOut) *errOut = "Connecte ton compte Team Launcher.";
        return false;
    }
    return true;
}

// --- worker (long-polling + rafraichissements) --------------------------------

void refresh_friends_locked(const std::string& b, const std::string& tok,
                            std::atomic<bool>* cancel) {
    const auto r = http::get_response(b + "/api/friends",
                                      "Authorization: Bearer " + tok + "\r\n",
                                      cancel);
    if (!r || r->status == 401) {
        if (r && r->status == 401) {
            std::lock_guard<std::mutex> lk(lock());
            ctx().authFailed = true;
        }
        return;
    }
    if (r->status != 200) return;
    try {
        const json j = json::parse(r->body);
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        c.friendsCache.clear();
        c.pendIn.clear();
        c.pendOut.clear();
        if (j.contains("friends") && j["friends"].is_array())
            for (const auto& f : j["friends"]) {
                Friend fr;
                fr.name = f.value("username", std::string{});
                fr.online = f.value("online", false);
                if (!fr.name.empty()) {
                    const auto uit = c.unread.find(fr.name);
                    fr.unread = (uit != c.unread.end()) ? uit->second : 0;
                    c.friendsCache.push_back(std::move(fr));
                }
            }
        if (j.contains("pendingIn") && j["pendingIn"].is_array())
            for (const auto& n : j["pendingIn"])
                if (n.is_string()) c.pendIn.push_back(n.get<std::string>());
        if (j.contains("pendingOut") && j["pendingOut"].is_array())
            for (const auto& n : j["pendingOut"])
                if (n.is_string()) c.pendOut.push_back(n.get<std::string>());
        alog("amis : " + std::to_string(c.friendsCache.size()) + " (" +
             std::to_string(c.pendIn.size()) + " demande(s))");
    } catch (const std::exception&) {
    }
}

void ingest_message_locked(const std::string& peer, long long id, bool mine,
                           const std::string& sender, const std::string& text) {
    auto& c = ctx();
    auto& thread = c.threads[peer];
    for (const auto& m : thread)
        if (!m.pending && m.id == id) return;
    Message m;
    m.id = id;
    m.mine = mine;
    m.sender = sender;
    m.text = text;
    m.unixTime = unix_now();
    thread.push_back(std::move(m));
    while (thread.size() > 200) thread.erase(thread.begin());
    if (!mine) c.unread[peer]++;
}

void handle_events_locked(const json& j) {
    auto& c = ctx();
    if (!j.contains("events") || !j["events"].is_array()) return;
    for (const auto& e : j["events"]) {
        const std::string type = e.value("type", std::string{});
        const long long id = e.value("id", 0LL);
        if (id > c.lastEventId) c.lastEventId = id;
        const json d = e.value("data", json::object());
        if (type == "message.new" && d.contains("message")) {
            const json m = d["message"];
            const std::string sender = m.value("sender", std::string{});
            ingest_message_locked(sender, m.value("id", 0LL), false, sender,
                                  m.value("text", std::string{}));
            for (auto& f : c.friendsCache)
                if (f.name == sender) {
                    f.unread++;
                    break;
                }
        } else if (type == "friend.request") {
            c.notice = "Demande d'ami de " +
                       d.value("from", json::object())
                           .value("username", std::string{"quelqu'un"}) +
                       ".";
        } else if (type == "friend.accepted") {
            c.notice = d.value("by", json::object())
                           .value("username", std::string{"Quelqu'un"}) +
                       " est maintenant ton ami.";
        } else if (type == "call.invite") {
            const std::string callId = d.value("callId", std::string{});
            const std::string lobby = d.value("lobbyId", std::string{});
            const std::string from = d.value("from", json::object())
                                         .value("username", std::string{});
            if (c.callDir != Ctx::CallDir::None || callId.empty() ||
                lobby.empty() || from.empty()) {
                continue; // occupe ou incomplet : l'appelant aura le timeout
            }
            c.callDir = Ctx::CallDir::In;
            c.callActive = false;
            c.apiCallId = callId;
            c.callPeer = from;
            c.callSecret = lobby;
            c.notice = "Appel entrant de " + from + ".";
        } else if (type == "call.accept") {
            const std::string callId = d.value("callId", std::string{});
            if (c.callDir == Ctx::CallDir::Out && !c.callActive &&
                c.apiCallId == callId && !callId.empty()) {
                c.callActive = true;
                c.callSince = unix_now();
                c.callDeadline = 0;
            }
        } else if (type == "call.decline") {
            const std::string callId = d.value("callId", std::string{});
            if (c.callDir == Ctx::CallDir::Out && c.apiCallId == callId &&
                !callId.empty()) {
                const std::uint64_t lobby = c.callLobby;
                reset_call_locked(c);
                c.callNotice = "Appel refusé.";
                if (lobby != 0) social::voice_leave(lobby);
            }
        } else if (type == "call.hangup") {
            const std::string callId = d.value("callId", std::string{});
            if (c.callDir != Ctx::CallDir::None && c.apiCallId == callId &&
                !callId.empty()) {
                const std::uint64_t lobby = c.callLobby;
                reset_call_locked(c);
                c.callNotice = "Appel terminé.";
                if (lobby != 0) social::voice_leave(lobby);
            }
        }
    }
}

void worker_main(std::shared_ptr<std::atomic<bool>> cancel) {
    alog("worker demarre");
    long long friendsAt = 0;
    for (;;) {
        if (cancel->load()) return;
        const std::string b = base();
        const std::string tok = token();
        if (b.empty() || tok.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        const std::string headers = "Authorization: Bearer " + tok + "\r\n";
        long long since = 0;
        {
            std::lock_guard<std::mutex> lk(lock());
            since = ctx().lastEventId;
        }
        // Le serveur garde la requete ~25 s ; le transport coupe a 60 s.
        const auto r = http::get_response(
            b + "/api/events?since=" + std::to_string(since), headers,
            cancel.get());
        if (cancel->load()) return;
        if (!r) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            continue;
        }
        if (r->status == 401) {
            std::lock_guard<std::mutex> lk(lock());
            ctx().authFailed = true;
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        if (r->status == 200) {
            bool needFriends = false;
            try {
                const json j = json::parse(r->body);
                const size_t nEvents =
                    j.value("events", json::array()).size();
                if (nEvents > 0)
                    alog("evenements recus : " + std::to_string(nEvents));
                std::lock_guard<std::mutex> lk(lock());
                const long long before = ctx().lastEventId;
                handle_events_locked(j);
                // Les evenements d'amitie changent la liste : rafraichir.
                needFriends = ctx().lastEventId != before;
                for (const auto& e : j.value("events", json::array())) {
                    const std::string t = e.value("type", std::string{});
                    if (t == "friend.request" || t == "friend.accepted")
                        needFriends = true;
                }
            } catch (const std::exception&) {
            }
            if (needFriends)
                refresh_friends_locked(b, tok, cancel.get());
        }
        const long long now = unix_now();
        if (now - friendsAt > 60) {
            friendsAt = now;
            refresh_friends_locked(b, tok, cancel.get());
        }
    }
}

} // namespace

Status status() {
    Status s;
    s.configured = !base().empty();
    std::lock_guard<std::mutex> lk(lock());
    auto& c = ctx();
    s.loggedIn =
        s.configured && !token().empty() && c.sessionOk && !c.authFailed;
    s.username = DataStore::settings.tlApiUser;
    if (!s.configured)
        s.detail = "Compte Team Launcher non configuré (URL d'API).";
    else if (c.authFailed || (!token().empty() && !c.sessionOk))
        s.detail = !c.error.empty() ? c.error : "Session expirée, reconnecte-toi.";
    else if (!s.loggedIn)
        s.detail = "Connecte ton compte Team Launcher.";
    else
        s.detail.clear();
    return s;
}

std::string take_notice() {
    std::lock_guard<std::mutex> lk(lock());
    std::string n = ctx().notice;
    ctx().notice.clear();
    return n;
}

bool register_account(const std::string& username, const std::string& password,
                      std::string* errOut) {
    if (base().empty()) {
        if (errOut)
            *errOut = "Renseigne d'abord l'URL de l'API dans les réglages.";
        return false;
    }
    if (username.size() < 3 || password.size() < 8) {
        if (errOut) *errOut = "Pseudo (3+) et mot de passe (8+) requis.";
        return false;
    }
    json body;
    body["username"] = username;
    body["password"] = password;
    const auto r = api_call("POST", "/api/accounts/register", body.dump(), false);
    if (!r) {
        if (errOut) *errOut = "Serveur injoignable.";
        return false;
    }
    json out;
    if (!parse_api(*r, out, errOut)) return false;
    alog("inscription ok : " + username);
    {
        std::lock_guard<std::mutex> lk(lock());
        DataStore::settings.tlApiToken =
            out.value("token", std::string{});
        DataStore::settings.tlApiUser =
            out.value("user", json::object())
                .value("username", username);
        DataStore::save();
        ctx().sessionOk = true;
        ctx().authFailed = false;
        ctx().error.clear();
        ctx().lastEventId = 0;
    }
    return true;
}

bool login(const std::string& username, const std::string& password,
           std::string* errOut) {
    if (base().empty()) {
        if (errOut)
            *errOut = "Renseigne d'abord l'URL de l'API dans les réglages.";
        return false;
    }
    if (username.empty() || password.empty()) {
        if (errOut) *errOut = "Pseudo et mot de passe requis.";
        return false;
    }
    json body;
    body["username"] = username;
    body["password"] = password;
    const auto r = api_call("POST", "/api/accounts/login", body.dump(), false);
    if (!r) {
        if (errOut) *errOut = "Serveur injoignable.";
        return false;
    }
    json out;
    if (!parse_api(*r, out, errOut)) return false;
    alog("connexion ok : " + username);
    {
        std::lock_guard<std::mutex> lk(lock());
        DataStore::settings.tlApiToken =
            out.value("token", std::string{});
        DataStore::settings.tlApiUser =
            out.value("user", json::object())
                .value("username", username);
        DataStore::save();
        ctx().sessionOk = true;
        ctx().authFailed = false;
        ctx().error.clear();
        ctx().lastEventId = 0;
    }
    return true;
}

void logout() {
    api_call("POST", "/api/accounts/logout", "{}", true);
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        DataStore::settings.tlApiToken.clear();
        DataStore::settings.tlApiUser.clear();
        DataStore::save();
        c.sessionOk = false;
        c.authFailed = false;
        c.threads.clear();
        c.unread.clear();
        c.historyAsked.clear();
        c.friendsCache.clear();
        c.pendIn.clear();
        c.pendOut.clear();
        const std::uint64_t lobby = c.callLobby;
        reset_call_locked(c);
        if (lobby != 0) social::voice_leave(lobby);
    }
    stop();
}

std::vector<Friend> friends() {
    std::lock_guard<std::mutex> lk(lock());
    return ctx().friendsCache;
}

std::vector<std::string> pending_in() {
    std::lock_guard<std::mutex> lk(lock());
    return ctx().pendIn;
}

std::vector<std::string> pending_out() {
    std::lock_guard<std::mutex> lk(lock());
    return ctx().pendOut;
}

namespace {
// POST ami + rafraichissement. Appel synchrone (fil d'UI, reponses courtes).
bool friend_post(const std::string& path, const json& body,
                 std::string* errOut) {
    if (!need_auth(errOut)) return false;
    const auto r = api_call("POST", path, body.dump(), true);
    if (!r) {
        if (errOut) *errOut = "Serveur injoignable.";
        return false;
    }
    json out;
    if (!parse_api(*r, out, errOut)) return false;
    return true;
}
} // namespace

bool friend_request(const std::string& username, std::string* errOut) {
    if (username.empty()) {
        if (errOut) *errOut = "Indique un pseudo.";
        return false;
    }
    json body;
    body["username"] = username;
    return friend_post("/api/friends/request", body, errOut);
}

bool friend_accept(const std::string& username, std::string* errOut) {
    json body;
    body["username"] = username;
    return friend_post("/api/friends/accept", body, errOut);
}

bool friend_decline(const std::string& username, std::string* errOut) {
    json body;
    body["username"] = username;
    return friend_post("/api/friends/decline", body, errOut);
}

bool friend_remove(const std::string& username, std::string* errOut) {
    if (!need_auth(errOut)) return false;
    // DELETE via POST + override (le transport ne fait que GET/POST bruts
    // avec corps ; le serveur comprend l'override).
    const auto r = api_call("DELETE", "/api/friends/" + username, "", true);
    if (!r) {
        if (errOut) *errOut = "Serveur injoignable.";
        return false;
    }
    json out;
    return parse_api(*r, out, errOut);
}

bool friend_block(const std::string& username, std::string* errOut) {
    json body;
    body["username"] = username;
    return friend_post("/api/friends/block", body, errOut);
}

std::vector<Message> conversation(const std::string& peer) {
    if (!need_auth(nullptr) || peer.empty()) return {};
    bool askHistory = false;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        c.unread[peer] = 0; // fil affiche = lu
        for (auto& f : c.friendsCache)
            if (f.name == peer) {
                f.unread = 0;
                break;
            }
        if (c.historyAsked.insert(peer).second) askHistory = true;
    }
    if (askHistory) {
        const auto r = api_call("GET", "/api/messages/" + peer + "?limit=50",
                                "", true);
        if (r && r->status != 401) {
            try {
                const json j = json::parse(r->body);
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                auto& thread = c.threads[peer];
                for (const auto& m : j.value("messages", json::array())) {
                    const long long id = m.value("id", 0LL);
                    bool dup = false;
                    for (const auto& t : thread)
                        if (!t.pending && t.id == id) {
                            dup = true;
                            break;
                        }
                    if (dup) continue;
                    Message msg;
                    msg.id = id;
                    msg.mine = m.value("mine", false);
                    msg.sender = m.value("sender", std::string{});
                    msg.text = m.value("text", std::string{});
                    msg.unixTime = unix_now();
                    thread.push_back(std::move(msg));
                }
                while (thread.size() > 200) thread.erase(thread.begin());
            } catch (const std::exception&) {
            }
        }
    }
    std::lock_guard<std::mutex> lk(lock());
    const auto it = ctx().threads.find(peer);
    if (it == ctx().threads.end()) return {};
    return it->second;
}

bool send(const std::string& peer, const std::string& text,
          std::string* errOut) {
    if (!need_auth(errOut)) return false;
    if (peer.empty() || text.empty()) {
        if (errOut) *errOut = "Message vide.";
        return false;
    }
    std::string localId;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        Message m;
        m.id = -(++c.pendingSeq); // negatif = local, jamais confondu
        m.mine = true;
        m.sender = DataStore::settings.tlApiUser;
        m.text = text;
        m.unixTime = unix_now();
        m.pending = true;
        localId = std::to_string(m.id);
        c.threads[peer].push_back(std::move(m));
        (void)localId;
    }
    json body;
    body["to"] = peer;
    body["text"] = text;
    const auto r = api_call("POST", "/api/messages", body.dump(), true);
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        auto it = c.threads.find(peer);
        if (it != c.threads.end()) {
            auto& thread = it->second;
            for (auto m = thread.begin(); m != thread.end(); ++m) {
                if (!m->pending || !m->mine) continue;
                if (m->text != text) continue;
                if (r && r->status == 201) {
                    try {
                        const json j = json::parse(r->body);
                        m->id = j.value("id", m->id);
                    } catch (const std::exception&) {
                    }
                    m->pending = false;
                } else {
                    thread.erase(m);
                    json out;
                    if (r) {
                        parse_api(*r, out, &c.sendError);
                        if (c.sendError.empty())
                            c.sendError = "Échec d'envoi. Réessaie.";
                    } else {
                        c.sendError = "Serveur injoignable.";
                    }
                    if (errOut) *errOut = c.sendError;
                    return false;
                }
                break;
            }
        }
    }
    return true;
}

std::string take_send_error() {
    std::lock_guard<std::mutex> lk(lock());
    std::string e = ctx().sendError;
    ctx().sendError.clear();
    return e;
}

CallInfo call_info() {
    CallInfo i;
    std::lock_guard<std::mutex> lk(lock());
    const auto& c = ctx();
    i.idle = (c.callDir == Ctx::CallDir::None);
    i.outgoing = (c.callDir == Ctx::CallDir::Out && !c.callActive);
    i.incoming = (c.callDir == Ctx::CallDir::In && !c.callActive);
    i.active = c.callActive;
    i.peer = c.callPeer;
    i.muted = c.callMuted;
    i.deaf = c.callDeaf;
    i.denoise = c.callDenoise;
    i.startedUnix = c.callSince;
    return i;
}

std::string take_call_notice() {
    std::lock_guard<std::mutex> lk(lock());
    std::string n = ctx().callNotice;
    ctx().callNotice.clear();
    return n;
}

bool call_start(const std::string& peer, std::string* errOut) {
    if (!need_auth(errOut)) return false;
    if (!social::status().ready) {
        if (errOut)
            *errOut = "Lie aussi Discord pour la voix (l'audio passe par Discord).";
        return false;
    }
    std::string secret;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.callDir != Ctx::CallDir::None) {
            if (errOut) *errOut = "Appel déjà en cours.";
            return false;
        }
        secret = make_secret();
        c.callDir = Ctx::CallDir::Out;
        c.callPeer = peer;
        c.callSecret = secret;
    }
    // 1. lobby vocal, 2. invitation via l'API (fil detache : ne bloque pas
    // l'interface). Tout echec nettoie le lobby.
    social::voice_join_async(
        secret, [peer, secret](std::uint64_t lobby, const std::string& error) {
            if (lobby == 0) {
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir == Ctx::CallDir::Out && c.callSecret == secret) {
                    reset_call_locked(c);
                    c.callNotice =
                        error.empty() ? "Vocal indisponible." : error;
                }
                return;
            }
            std::thread([peer, secret, lobby]() {
                json body;
                body["to"] = peer;
                body["lobbyId"] = std::to_string(lobby);
                const auto r =
                    api_call("POST", "/api/calls/invite", body.dump(), true);
                std::lock_guard<std::mutex> lk(lock());
                auto& c = ctx();
                if (c.callDir != Ctx::CallDir::Out || c.callSecret != secret)
                    return; // annule entre-temps
                if (!r) {
                    social::voice_leave(lobby);
                    reset_call_locked(c);
                    c.callNotice = "Serveur injoignable.";
                    return;
                }
                json out;
                std::string err;
                if (!parse_api(*r, out, &err)) {
                    social::voice_leave(lobby);
                    reset_call_locked(c);
                    c.callNotice = err.empty() ? "Invitation impossible." : err;
                    return;
                }
                c.apiCallId = out.value("callId", std::string{});
                c.callLobby = lobby;
                c.callDeadline = unix_now() + 90;
                c.callDenoise = true;
            }).detach();
        });
    return true;
}

bool call_accept(std::string* errOut) {
    if (!need_auth(errOut)) return false;
    if (!social::status().ready) {
        if (errOut)
            *errOut = "Lie aussi Discord pour la voix (l'audio passe par Discord).";
        return false;
    }
    std::string apiId, lobbyStr;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.callDir != Ctx::CallDir::In || c.apiCallId.empty()) {
            if (errOut) *errOut = "Aucun appel entrant.";
            return false;
        }
        apiId = c.apiCallId;
        lobbyStr = c.callSecret;
    }
    json body;
    const auto r =
        api_call("POST", "/api/calls/" + apiId + "/accept", body.dump(), true);
    if (!r) {
        if (errOut) *errOut = "Serveur injoignable.";
        return false;
    }
    json out;
    if (!parse_api(*r, out, errOut)) return false;
    social::voice_join_async(
        lobbyStr, [apiId](std::uint64_t lobby, const std::string& error) {
            std::lock_guard<std::mutex> lk(lock());
            auto& c = ctx();
            if (c.callDir != Ctx::CallDir::In || c.apiCallId != apiId) {
                if (lobby != 0) social::voice_leave(lobby);
                return;
            }
            if (lobby == 0) {
                reset_call_locked(c);
                c.callNotice =
                    error.empty() ? "Vocal indisponible." : error;
                // Prevenir l'appelant : sans ca il sonne dans le vide.
                std::string id = apiId;
                std::thread([id]() {
                    json b;
                    api_call("POST", "/api/calls/" + id + "/hangup",
                             b.dump(), true);
                }).detach();
                return;
            }
            c.callLobby = lobby;
            c.callActive = true;
            c.callSince = unix_now();
            c.callDenoise = true;
        });
    return true;
}

bool call_decline(std::string* errOut) {
    if (!need_auth(errOut)) return false;
    std::string apiId;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.callDir != Ctx::CallDir::In || c.apiCallId.empty()) {
            if (errOut) *errOut = "Aucun appel entrant.";
            return false;
        }
        apiId = c.apiCallId;
        reset_call_locked(c);
    }
    json body;
    const auto r =
        api_call("POST", "/api/calls/" + apiId + "/decline", body.dump(), true);
    if (!r) {
        if (errOut) *errOut = "Serveur injoignable.";
        return false;
    }
    json out;
    return parse_api(*r, out, errOut);
}

bool call_hangup(std::string* errOut) {
    if (!need_auth(errOut)) return false;
    std::string apiId;
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.callDir == Ctx::CallDir::None) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
        apiId = c.apiCallId;
        lobby = c.callLobby;
        reset_call_locked(c);
    }
    if (lobby != 0) social::voice_leave(lobby);
    if (!apiId.empty()) {
        std::thread([apiId]() {
            json b;
            api_call("POST", "/api/calls/" + apiId + "/hangup", b.dump(), true);
        }).detach();
    }
    return true;
}

bool call_set_muted(bool muted, std::string* errOut) {
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        lobby = ctx().callLobby;
        if (lobby == 0) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
    }
    if (!social::voice_set_muted(lobby, muted, errOut)) return false;
    std::lock_guard<std::mutex> lk(lock());
    ctx().callMuted = muted;
    return true;
}

bool call_set_deaf(bool deaf, std::string* errOut) {
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        lobby = ctx().callLobby;
        if (lobby == 0) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
    }
    if (!social::voice_set_deaf(lobby, deaf, errOut)) return false;
    std::lock_guard<std::mutex> lk(lock());
    ctx().callDeaf = deaf;
    return true;
}

bool call_set_denoise(bool denoise, std::string* errOut) {
    std::uint64_t lobby = 0;
    {
        std::lock_guard<std::mutex> lk(lock());
        lobby = ctx().callLobby;
        if (lobby == 0) {
            if (errOut) *errOut = "Aucun appel en cours.";
            return false;
        }
    }
    if (!social::voice_set_denoise(lobby, denoise, errOut)) return false;
    std::lock_guard<std::mutex> lk(lock());
    ctx().callDenoise = denoise;
    return true;
}

void start() {
    // JAMAIS de HTTP sous verrou : les helpers (refresh_friends_locked...)
    // prennent le verrou eux-memes (mutex non recursif).
    std::string b, tok;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.workerOn) return;
        b = base();
        tok = token();
        if (b.empty() || tok.empty()) return; // rien a faire sans URL ni jeton
        cancel = std::make_shared<std::atomic<bool>>(false);
        c.cancel = cancel;
    }
    alog("demarrage, base=" + b);
    http::allow_host(b);
    alog(std::string("allowlist 127.0.0.1=") + (http::host_allowed("127.0.0.1") ? "oui" : "non"));
    const std::string headers = "Authorization: Bearer " + tok + "\r\n";
    // Validation de la session (rapide en local/LAN).
    alog("validation session...");
    const auto r = http::get_response(b + "/api/me", headers, nullptr);
    alog(std::string("validation -> ") + (r ? std::to_string(r->status) : std::string("reseau")));
    if (!r || r->status == 401) {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (r && r->status == 401) {
            c.authFailed = true;
            c.error = "Session expirée, reconnecte-toi.";
        } else {
            c.error = "Serveur injoignable.";
        }
        c.sessionOk = false;
        return;
    }
    if (r->status != 200) {
        std::lock_guard<std::mutex> lk(lock());
        ctx().sessionOk = false;
        ctx().error = "Serveur injoignable.";
        return;
    }
    try {
        const json j = json::parse(r->body);
        if (j.contains("user") && j["user"].contains("username"))
            DataStore::settings.tlApiUser =
                j["user"].value("username", std::string{});
    } catch (const std::exception&) {
    }
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        c.sessionOk = true;
        c.authFailed = false;
        c.error.clear();
    }
    // Presence : en ligne (le long-polling entretient last_seen).
    alog("presence...");
    http::post_string(b + "/api/presence", "{\"status\":\"online\"}",
                      "application/json", headers, nullptr);
    alog("presence ok");
    // Historique immediat des amis, puis worker.
    alog("amis...");
    refresh_friends_locked(b, tok, nullptr);
    alog("amis ok");
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.workerOn) return; // course improbable : un worker tourne deja
        c.workerOn = true;
        c.worker = std::thread([cancel]() { worker_main(cancel); });
    }
    alog("worker lance");
}

void stop() {
    std::unique_lock<std::mutex> lk(lock());
    auto& c = ctx();
    // Presence : hors ligne (best effort, synchrone et bref).
    const std::string b = base();
    const bool hasSession = !token().empty() && c.sessionOk;
    lk.unlock();
    if (!b.empty() && hasSession) {
        http::post_string(b + "/api/presence", "{\"status\":\"offline\"}",
                          "application/json", auth_headers(), nullptr);
    }
    lk.lock();
    if (c.workerOn) {
        if (c.cancel) c.cancel->store(true);
        std::thread w = std::move(c.worker);
        c.workerOn = false;
        lk.unlock();
        if (w.joinable()) w.join();
        lk.lock();
    }
    const std::uint64_t lobby = c.callLobby;
    reset_call_locked(c);
    c.threads.clear();
    c.unread.clear();
    c.historyAsked.clear();
    c.friendsCache.clear();
    lk.unlock();
    if (lobby != 0) social::voice_leave(lobby);
}

void pump() {
    // Jeton rejete par le serveur : nettoyage sur le thread d'UI (le seul
    // qui persiste les reglages).
    {
        std::lock_guard<std::mutex> lk(lock());
        auto& c = ctx();
        if (c.authFailed && !token().empty()) {
            DataStore::settings.tlApiToken.clear();
            DataStore::settings.tlApiUser.clear();
            DataStore::save();
            c.authFailed = false;
            c.sessionOk = false;
            c.notice = "Session expirée, reconnecte-toi.";
        }
        // Timeout d'appel sortant sans acceptation.
        if (c.callDir == Ctx::CallDir::Out && !c.callActive &&
            c.callDeadline != 0 && unix_now() > c.callDeadline) {
            const std::string apiId = c.apiCallId;
            const std::uint64_t lobby = c.callLobby;
            reset_call_locked(c);
            c.callNotice = "Sans réponse.";
            if (!apiId.empty()) {
                std::thread([apiId]() {
                    json bo;
                    api_call("POST", "/api/calls/" + apiId + "/hangup",
                             bo.dump(), true);
                }).detach();
            }
            if (lobby != 0) social::voice_leave(lobby);
        }
    }
}

} // namespace tl::account

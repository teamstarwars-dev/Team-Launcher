// Faux plugin pour TLTestPluginHost. Parle le protocole V8 sur ses flux
// standard, et rend compte de ce qu'il observe par log.write — que le test
// lit côté launcher.
//
// Modes (premier argument) :
//   normal  : hello, instances.list (permise), game.launch (refusée), fin
//   nohello : appelle avant de se présenter, puis se présente
//   garbage : envoie une longueur de trame mensongère
//   probe   : sonde une permission non accordée jusqu'à se faire tuer
//   flood   : rafale d'appels pour déclencher la limite de débit
//   forge   : tente d'appeler une méthode hors table
//   idle    : se présente puis attend sans rien faire (cible du kill-switch)

#include "pipe_child.hpp"
#include "plugin_proto.hpp"

#include <chrono>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <vector>

using nlohmann::json;
namespace pp = tl::pluginproto;
namespace pc = tl::pipechild;

namespace {

pp::FrameReader g_in;

void send_raw(const std::string& payload) {
    const std::string f = pp::encode_frame(payload);
    pc::stdio_write(f.data(), f.size());
}

int g_id = 1;

std::int64_t call(const std::string& method, const json& params = json::object()) {
    const int id = g_id++;
    send_raw(json{{"v", 1}, {"id", id}, {"method", method}, {"params", params}}.dump());
    return id;
}

// Attend la réponse portant cet id ; rend l'objet réponse, ou null si le
// flux se ferme.
// Les réponses arrivées pour un AUTRE id sont gardées de côté : en
// rafale, les refus de débit reviennent avant les réponses servies, et
// les jeter ferait attendre indéfiniment une réponse déjà reçue.
std::map<std::int64_t, json> g_stash;

json wait_reply(std::int64_t id) {
    std::vector<char> buf(8192);
    for (;;) {
        if (auto it = g_stash.find(id); it != g_stash.end()) {
            json j = it->second;
            g_stash.erase(it);
            return j;
        }
        std::string frame;
        while (g_in.next(&frame) == pp::FrameReader::Status::Frame) {
            const json j = json::parse(frame, nullptr, false);
            if (!j.is_object()) continue;
            const auto rid = j.value("id", std::int64_t{-1});
            if (rid == id) return j;
            g_stash[rid] = j;
        }
        const long n = pc::stdio_read(buf.data(), buf.size());
        if (n <= 0) return json();
        g_in.feed(buf.data(), static_cast<std::size_t>(n));
    }
}

int error_code(const json& r) {
    if (r.is_object() && r.contains("error")) return r["error"].value("code", 0);
    return 0;
}

void report(const std::string& msg) {
    wait_reply(call("log.write", {{"level", "info"}, {"message", msg}}));
}

void hello() { wait_reply(call("host.hello", {{"api", 1}})); }

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "normal";

    if (mode == "normal") {
        hello();
        const json l = wait_reply(call("instances.list"));
        report("LIST n=" + std::to_string(l["result"]["instances"].size()));
        const json g = wait_reply(call("game.launch", {{"instanceId", std::string(32, 'a')}}));
        report("DENIED code=" + std::to_string(error_code(g)));
        report("FIN");
        return 0;
    }
    if (mode == "nohello") {
        const json r = wait_reply(call("instances.list"));
        const int code = error_code(r);
        hello();
        report("NOHELLO code=" + std::to_string(code));
        return 0;
    }
    if (mode == "garbage") {
        hello();
        const char lie[4] = {static_cast<char>(0xFF), static_cast<char>(0xFF),
                             static_cast<char>(0xFF), static_cast<char>(0x7F)};
        pc::stdio_write(lie, 4);
        // Le host doit nous tuer ; on attend sans rien faire.
        std::this_thread::sleep_for(std::chrono::seconds(10));
        return 0;
    }
    if (mode == "probe") {
        hello();
        for (int i = 0; i < 100; ++i) {
            const json r = wait_reply(call("game.launch", {{"instanceId", std::string(32, 'b')}}));
            if (r.is_null()) return 0; // tué : c'est le résultat attendu
        }
        report("PROBE survived");
        return 0;
    }
    if (mode == "flood") {
        hello();
        // On envoie sans attendre de réponse : le seau doit se vider.
        std::vector<std::int64_t> ids;
        for (int i = 0; i < 120; ++i) ids.push_back(call("instances.list"));
        int limited = 0;
        for (const auto id : ids)
            if (error_code(wait_reply(id)) == pp::kRateLimited) ++limited;
        // Le rapport passe lui aussi par la limite de débit : laisser le
        // seau se recharger, sinon il serait refusé et le test aveugle.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        report("FLOOD limited=" + std::to_string(limited));
        return 0;
    }
    if (mode == "idle") {
        hello();
        std::this_thread::sleep_for(std::chrono::seconds(20));
        report("IDLE survived");
        return 0;
    }
    if (mode == "forge") {
        hello();
        const json r = wait_reply(call("accounts.token"));
        report("FORGE code=" + std::to_string(error_code(r)));
        return 0;
    }
    return 2;
}

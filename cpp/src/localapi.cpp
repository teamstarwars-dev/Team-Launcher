#include "localapi.hpp"

#include "apikeys.hpp"
#include "datastore.hpp" // new_guid (aléatoire du système)
#include "diagapi.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <thread>

// cpp-httplib est déjà fourni (il servait d'en-tête de référence pour la
// couche sortante). Il est inclus ICI et nulle part ailleurs : c'est un
// en-tête énorme, et une seule unité de compilation doit le payer.
#ifndef CPPHTTPLIB_NO_EXCEPTIONS
#define CPPHTTPLIB_THREAD_POOL_COUNT 2
#endif
#include <httplib.h>

namespace tl::localapi {

namespace {

struct Ctx {
    std::mutex m;
    std::thread th;
    httplib::Server* srv = nullptr;
    bool up = false;
    int port = 0;
    std::string token;
    std::string error;
    std::string statusJson = "{}";
    std::string instancesJson = "[]";
    std::vector<std::string> launches;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

// Comparaison à durée constante. Un `==` sur des chaînes s'arrête au
// premier octet différent, ce qui laisse mesurer combien de caractères
// sont bons — sur une boucle locale, c'est assez pour retrouver un jeton
// octet par octet.
bool token_equal(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

// Secret présenté, ou vide.
std::string bearer(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return {};
    const std::string& v = it->second;
    constexpr const char* kPrefix = "Bearer ";
    if (v.rfind(kPrefix, 0) != 0) return {};
    return v.substr(7);
}

// Résultat d'un contrôle d'accès : qui appelle, et pourquoi c'est refusé.
struct Auth {
    bool ok = false;
    bool scopeDenied = false;
    std::string who; // nom de l'application, pour le journal
};

// Deux chemins coexistent, et c'est voulu :
//   - le JETON UNIQUE, celui des réglages, garde tous les droits. Il est
//     l'outil de l'utilisateur sur sa propre machine.
//   - les CLÉS D'APPLICATION, distribuées, n'ont que leurs portées.
Auth authorise(const httplib::Request& req, const std::string& ownerToken,
               const std::string& scope) {
    Auth a;
    const std::string presented = bearer(req);
    if (presented.empty()) return a;
    if (!ownerToken.empty() && token_equal(presented, ownerToken)) {
        a.ok = true;
        a.who = "propriétaire";
        return a;
    }
    const auto m = apikeys::check(presented, scope);
    a.ok = m.ok;
    a.scopeDenied = m.scopeDenied;
    a.who = m.appName;
    return a;
}

} // namespace

std::string make_token() {
    // 32 hexa tirés du générateur du système (BCryptGenRandom / getrandom),
    // comme les identifiants d'instance. Deux, pour 256 bits.
    return new_guid() + new_guid();
}

void publish(const std::string& statusJson, const std::string& instancesJson) {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    c.statusJson = statusJson;
    c.instancesJson = instancesJson;
}

std::vector<std::string> take_launch_requests() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    std::vector<std::string> out;
    out.swap(c.launches);
    return out;
}

bool start(int port, const std::string& token, std::string* errOut) {
    auto& c = ctx();
    auto fail = [&](const std::string& m) {
        {
            std::lock_guard<std::mutex> lk(c.m);
            c.error = m;
        }
        if (errOut) *errOut = m;
        return false;
    };
    if (port <= 0 || port > 65535) return fail("Port invalide.");
    if (token.empty())
        return fail("Aucun jeton : l'API ne démarre pas sans authentification.");
    stop();

    auto* srv = new httplib::Server();
    {
        std::lock_guard<std::mutex> lk(c.m);
        c.srv = srv;
        c.token = token;
        c.port = port;
        c.error.clear();
    }

    // Le jeton est relu à chaque requête plutôt que capturé : on peut le
    // régénérer sans redémarrer le serveur.
    // Le jeton est relu à chaque requête plutôt que capturé : on peut le
    // régénérer sans redémarrer le serveur.
    auto guard = [](const httplib::Request& req, httplib::Response& res,
                    const char* scope) {
        std::string tok;
        {
            auto& cc = ctx();
            std::lock_guard<std::mutex> lk(cc.m);
            tok = cc.token;
        }
        const Auth a = authorise(req, tok, scope);
        if (a.ok) return true;
        // 403 et non 401 quand la clé est bonne mais la portée absente :
        // l'appelant doit savoir qu'il ne s'agit pas de se réauthentifier
        // mais de demander un droit qu'on ne lui a pas donné.
        res.status = a.scopeDenied ? 403 : 401;
        res.set_content(
            a.scopeDenied
                ? std::string("{\"error\":\"portée « ") + scope +
                      " » absente de cette clé\"}"
                : std::string("{\"error\":\"clé manquante ou invalide\"}"),
            "application/json");
        return false;
    };

    auto statusH = [guard](const httplib::Request& req,
                           httplib::Response& res) {
        if (!guard(req, res, apikeys::kScopeRead)) return;
        auto& c2 = ctx();
        std::lock_guard<std::mutex> lk(c2.m);
        res.set_content(c2.statusJson, "application/json");
    };
    srv->Get("/v1/status", statusH);
    srv->Get("/api/status", statusH); // ancien chemin, conservé

    auto instancesH = [guard](const httplib::Request& req,
                              httplib::Response& res) {
        if (!guard(req, res, apikeys::kScopeRead)) return;
        auto& c2 = ctx();
        std::lock_guard<std::mutex> lk(c2.m);
        res.set_content(c2.instancesJson, "application/json");
    };
    srv->Get("/v1/instances", instancesH);
    srv->Get("/api/instances", instancesH);

    // --- Diagnostic : les deux capacités qui n'accèdent à RIEN de local --
    // Elles ne lisent aucun fichier, ne touchent pas au réseau et ne
    // voient aucune donnée de l'utilisateur : tout arrive dans le corps de
    // la requête. C'est ce qui les rend distribuables, et un jour
    // hébergeables, contrairement au reste.
    //
    // Le travail est délégué à `diagapi`, partagé avec le service
    // autonome `tl_diagd` : deux copies de ces gestionnaires auraient
    // fini par rendre deux réponses différentes à la même question.
    srv->Post("/v1/diag/crash", [guard](const httplib::Request& req,
                                        httplib::Response& res) {
        if (!guard(req, res, apikeys::kScopeDiag)) return;
        const auto r = diagapi::crash(req.body);
        res.status = r.status;
        res.set_content(r.body, "application/json");
    });

    srv->Post("/v1/diag/mods", [guard](const httplib::Request& req,
                                       httplib::Response& res) {
        if (!guard(req, res, apikeys::kScopeDiag)) return;
        const auto r = diagapi::mods(req.body);
        res.status = r.status;
        res.set_content(r.body, "application/json");
    });

    // Auto-description : ce que la clé présentée permet de faire.
    srv->Get("/v1", [guard](const httplib::Request& req,
                            httplib::Response& res) {
        if (!guard(req, res, "")) return;
        res.set_content(diagapi::describe(), "application/json");
    });

    auto launchH = [guard](const httplib::Request& req,
                           httplib::Response& res) {
        if (!guard(req, res, apikeys::kScopeControl)) return;
        // Le corps n'est pas interprété comme du JSON ici pour ne rien
        // exécuter sur ce fil : on en extrait l'identifiant, et c'est la
        // boucle de l'interface qui vérifiera qu'il existe.
        std::string id;
        const std::string& b = req.body;
        const size_t k = b.find("\"id\"");
        if (k != std::string::npos) {
            size_t q1 = b.find('"', b.find(':', k) + 1);
            if (q1 != std::string::npos) {
                const size_t q2 = b.find('"', q1 + 1);
                if (q2 != std::string::npos) id = b.substr(q1 + 1, q2 - q1 - 1);
            }
        }
        if (id.empty()) {
            res.status = 400;
            res.set_content("{\"error\":\"corps attendu : {\\\"id\\\":\\\"…\\\"}\"}",
                            "application/json");
            return;
        }
        {
            auto& c2 = ctx();
            std::lock_guard<std::mutex> lk(c2.m);
            // Plafond : un appelant en boucle ne doit pas faire gonfler la
            // file indéfiniment.
            if (c2.launches.size() < 8) c2.launches.push_back(id);
        }
        res.set_content("{\"queued\":true}", "application/json");
    };
    srv->Post("/v1/launch", launchH);
    srv->Post("/api/launch", launchH);

    // Un démarrage raté (port pris) n'est connu qu'une fois `listen`
    // revenu : on le sait donc après coup, et l'interface lira last_error().
    std::thread th([srv, port] {
        const bool ok = srv->listen("127.0.0.1", port);
        auto& c2 = ctx();
        std::lock_guard<std::mutex> lk(c2.m);
        c2.up = false;
        if (!ok && c2.error.empty())
            c2.error = "Impossible d'écouter sur 127.0.0.1:" +
                       std::to_string(port) + " (port déjà utilisé ?).";
    });

    // `listen` ne rend la main qu'à l'arrêt : on attend que le serveur se
    // déclare prêt plutôt que de supposer qu'il l'est. wait_until_ready()
    // revient aussi quand l'écoute a ÉCHOUÉ (il attend « plus en train de
    // démarrer »), d'où le is_running() derrière : sans lui, un port déjà
    // pris passerait pour un démarrage réussi.
    srv->wait_until_ready();
    if (!srv->is_running()) {
        srv->stop();
        th.join();
        delete srv;
        std::lock_guard<std::mutex> lk(c.m);
        c.srv = nullptr;
        const std::string m = c.error.empty()
                                  ? "Démarrage de l'API locale impossible."
                                  : c.error;
        if (errOut) *errOut = m;
        return false;
    }

    std::lock_guard<std::mutex> lk(c.m);
    c.th = std::move(th);
    c.up = true;
    return true;
}

void stop() {
    auto& c = ctx();
    httplib::Server* srv = nullptr;
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(c.m);
        srv = c.srv;
        c.srv = nullptr;
        th = std::move(c.th);
        c.up = false;
    }
    if (srv) srv->stop();
    if (th.joinable()) th.join();
    delete srv;
}

bool running() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    return c.up;
}

int port() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    return c.up ? c.port : 0;
}

std::string last_error() {
    auto& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    return c.error;
}

} // namespace tl::localapi

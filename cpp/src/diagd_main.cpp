// tl_diagd — service de diagnostic Minecraft, autonome.
//
// Sert UNIQUEMENT les deux analyses : un log de crash en entrée, le mod
// coupable en sortie ; une liste de mods en entrée, les conflits en
// sortie. Rien d'autre. Pas de lecture d'instance, pas de lancement de
// partie : ces capacités-là demandent un launcher sur la machine, et un
// serveur n'en a pas.
//
// Il ne dépend que de `std`, nlohmann et miniz. Ni SDL, ni SDK Discord,
// ni configuration de launcher, ni base de données. C'est ce qui le rend
// hébergeable pour presque rien.
//
// DISCIPLINE CENTRALE, et la seule qui ne se rattrape pas : **le corps
// des requêtes n'est jamais conservé**. Un rapport de crash contient le
// nom de compte Windows dans les chemins de fichiers, parfois des noms de
// serveurs, parfois des dossiers personnels. L'analyse se fait en
// mémoire ; le journal ne reçoit que le verdict, l'identifiant de clé et
// la taille. Jamais le contenu, même en cas d'erreur, même en mode
// bavard.
//
// TLS : volontairement absent. Ce service s'installe derrière un reverse
// proxy (nginx, Caddy) qui termine le TLS. Embarquer OpenSSL ici
// ajouterait une dépendance lourde pour refaire moins bien ce qu'un
// proxy fait déjà.

#include "diagapi.hpp"
#include "util_hash.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include <httplib.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h> // getrandom
#endif

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

// --- Clés -------------------------------------------------------------
// Fichier JSON rechargé quand sa date de modification change : on ajoute
// une clé à la main sans redémarrer le service. C'est volontairement
// rudimentaire — il y aura cinq clés, pas cinq mille, et un système de
// comptes coûterait plus qu'il ne rapporterait.
struct Key {
    std::string id;
    std::string hash;      // SHA-1 du secret ; le secret n'est jamais rangé
    std::string appName;
    int ratePerMin = 60;
    bool revoked = false;
};

struct Usage {
    long long windowStart = 0; // minute en cours (epoch / 60)
    int count = 0;
    long long total = 0;
};

std::mutex g_lock;
std::vector<Key> g_keys;
std::map<std::string, Usage> g_usage;
fs::path g_keysPath;
fs::file_time_type g_keysStamp{};
std::atomic<bool> g_running{true};

long long now_unix() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

std::string stamp() {
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

// Journal : une ligne par requête, sur stdout, sans jamais le contenu.
void log_line(const std::string& text) {
    std::printf("%s %s\n", stamp().c_str(), text.c_str());
    std::fflush(stdout);
}

std::vector<Key> parse_keys(const std::string& text, std::string* errOut) {
    std::vector<Key> out;
    json j;
    try {
        j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& ex) {
        if (errOut) *errOut = ex.what();
        return out;
    }
    const json* arr = nullptr;
    if (j.is_array()) arr = &j;
    else if (j.is_object() && j.contains("keys") && j["keys"].is_array())
        arr = &j["keys"];
    if (!arr) {
        if (errOut) *errOut = "attendu : un tableau, ou {\"keys\":[...]}";
        return out;
    }
    for (const auto& e : *arr) {
        if (!e.is_object()) continue;
        Key k;
        k.id = e.value("id", std::string{});
        k.hash = e.value("hash", std::string{});
        k.appName = e.value("appName", k.id);
        k.ratePerMin = e.value("ratePerMin", 60);
        k.revoked = e.value("revoked", false);
        if (k.id.empty() || k.hash.empty()) continue;
        out.push_back(std::move(k));
    }
    return out;
}

// Relit le fichier si sa date a changé. Appelé à chaque requête : un
// `stat` par requête ne coûte rien à cette échelle, et évite d'avoir à
// redémarrer pour révoquer quelqu'un.
void refresh_keys_locked() {
    if (g_keysPath.empty()) return;
    std::error_code ec;
    const auto t = fs::last_write_time(g_keysPath, ec);
    if (ec || t == g_keysStamp) return;
    std::ifstream in(g_keysPath, std::ios::binary);
    if (!in) return;
    std::ostringstream ss;
    ss << in.rdbuf();
    std::string err;
    auto keys = parse_keys(ss.str(), &err);
    if (!err.empty()) {
        // Un fichier cassé ne doit PAS révoquer tout le monde : on garde
        // les clés en mémoire et on crie dans le journal.
        log_line("ERREUR clés illisibles, anciennes conservées : " + err);
        g_keysStamp = t;
        return;
    }
    g_keysStamp = t;
    g_keys = std::move(keys);
    log_line("clés rechargées : " + std::to_string(g_keys.size()));
}

std::string bearer(const httplib::Request& req) {
    auto it = req.headers.find("Authorization");
    if (it == req.headers.end()) return {};
    const std::string& v = it->second;
    if (v.rfind("Bearer ", 0) != 0) return {};
    return v.substr(7);
}

struct Auth {
    bool ok = false;
    bool rateLimited = false;
    std::string id = "-";
    std::string appName;
    int retryAfter = 0;
};

Auth authorise(const httplib::Request& req) {
    Auth a;
    const std::string secret = bearer(req);
    if (secret.empty()) return a;
    const std::string h = tl::sha1_hex_of(secret);

    std::lock_guard<std::mutex> lk(g_lock);
    refresh_keys_locked();
    for (const auto& k : g_keys) {
        // Comparaison à durée constante : un `==` s'arrête au premier
        // octet différent, ce qui laisse retrouver une clé caractère par
        // caractère en mesurant le temps de réponse.
        if (k.hash.size() != h.size()) continue;
        unsigned char diff = 0;
        for (size_t i = 0; i < h.size(); ++i)
            diff |= static_cast<unsigned char>(k.hash[i] ^ h[i]);
        if (diff != 0) continue;

        a.id = k.id;
        a.appName = k.appName;
        if (k.revoked) return a;

        auto& u = g_usage[k.id];
        const long long minute = now_unix() / 60;
        if (u.windowStart != minute) {
            u.windowStart = minute;
            u.count = 0;
        }
        if (k.ratePerMin > 0 && u.count >= k.ratePerMin) {
            a.rateLimited = true;
            a.retryAfter = static_cast<int>(60 - (now_unix() % 60));
            return a;
        }
        ++u.count;
        ++u.total;
        a.ok = true;
        return a;
    }
    return a;
}

void send_json(httplib::Response& res, int status, const std::string& body) {
    res.status = status;
    res.set_content(body, "application/json");
}

// Enveloppe commune : authentification, plafond de débit, journal.
// `fn` reçoit le corps et rend le résultat ; elle ne voit ni HTTP ni clé.
void handle(const char* route, const httplib::Request& req,
            httplib::Response& res,
            tl::diagapi::Result (*fn)(const std::string&)) {
    const auto a = authorise(req);
    if (!a.ok) {
        if (a.rateLimited) {
            res.set_header("Retry-After", std::to_string(a.retryAfter));
            send_json(res, 429,
                      R"({"error":"trop de requêtes, réessayez dans une minute"})");
            log_line(std::string(route) + " 429 key=" + a.id);
        } else {
            send_json(res, 401, R"({"error":"clé manquante, inconnue ou révoquée"})");
            log_line(std::string(route) + " 401 key=-");
        }
        return;
    }
    const auto r = fn(req.body);
    send_json(res, r.status, r.body);
    // Le journal porte la TAILLE du corps, jamais le corps. Et le verdict,
    // qui est précisément ce qu'on a le droit de savoir.
    std::string verdict;
    try {
        const auto j = json::parse(r.body);
        if (j.contains("cause")) verdict = " cause=" + j.value("cause", "");
        if (j.contains("errors"))
            verdict = " errors=" + std::to_string(j.value("errors", 0));
    } catch (const std::exception&) {
    }
    log_line(std::string(route) + " " + std::to_string(r.status) +
             " key=" + a.id + " bytes=" + std::to_string(req.body.size()) +
             verdict);
}

void print_usage() {
    std::printf(
        "tl_diagd — service de diagnostic Minecraft (Team Launcher %s)\n\n"
        "  tl_diagd --keys <fichier.json> [--host 127.0.0.1] [--port 8080]\n"
        "           [--max-body <octets, defaut 4194304>]\n"
        "  tl_diagd --new-key \"Nom de l'application\" [--rate 60]\n\n"
        "Routes : POST /v1/diag/crash, POST /v1/diag/mods, GET /v1,\n"
        "         GET /healthz (sans clé).\n\n"
        "Le TLS est assuré par un reverse proxy en amont, pas ici.\n"
        "Le corps des requêtes n'est jamais écrit sur disque.\n",
        TL_VERSION_STRING);
}

// Octets aléatoires du générateur du système. `std::random_device` aurait
// suffi en pratique, mais sa qualité est laissée à l'implémentation par
// la norme : pour fabriquer un secret, mieux vaut nommer explicitement la
// source plutôt que d'espérer.
bool random_bytes(unsigned char* out, size_t n) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(n),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    size_t done = 0;
    while (done < n) {
        const ssize_t r = getrandom(out + done, n - done, 0);
        if (r <= 0) return false;
        done += static_cast<size_t>(r);
    }
    return true;
#endif
}

std::string random_hex(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
    if (!random_bytes(buf.data(), buf.size())) return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(bytes * 2);
    for (unsigned char c : buf) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0x0F]);
    }
    return out;
}

// Génère une clé et imprime les deux moitiés : le secret à remettre au
// demandeur (une seule fois) et la ligne JSON à coller dans le fichier.
int new_key(const std::string& appName, int rate) {
    const std::string secret = random_hex(32); // 256 bits
    // L'identifiant est tiré SÉPARÉMENT du secret. Le dériver de ses
    // premiers caractères — ce que faisait la première version — le
    // faisait fuiter : l'identifiant est public, il apparaît dans le
    // journal, dans le fichier de clés et sur toute capture d'écran de
    // support. Quarante-huit bits donnés pour rien.
    const std::string id = "ak_" + random_hex(6);
    if (secret.empty() || id.size() < 5) {
        std::fprintf(stderr,
                     "générateur aléatoire du système indisponible : aucune "
                     "clé générée.\n");
        return 1;
    }
    json line{{"id", id},
              {"hash", tl::sha1_hex_of(secret)},
              {"appName", appName.empty() ? "Application sans nom" : appName},
              {"ratePerMin", rate},
              {"revoked", false}};
    std::printf("\nSecret à remettre au demandeur (affiché une seule fois) :\n"
                "  %s\n\n"
                "Ligne à ajouter au tableau \"keys\" du fichier :\n"
                "  %s\n\n",
                secret.c_str(), line.dump().c_str());
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 8080;
    std::string keysFile;
    std::string newKeyName;
    int rate = 60;
    size_t maxBody = 4u * 1024 * 1024;
    bool wantNewKey = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            return i + 1 < argc ? std::string(argv[++i]) : std::string();
        };
        if (a == "--host") host = next();
        else if (a == "--port") port = std::atoi(next().c_str());
        else if (a == "--keys") keysFile = next();
        else if (a == "--max-body") maxBody = static_cast<size_t>(
                     std::strtoull(next().c_str(), nullptr, 10));
        else if (a == "--rate") rate = std::atoi(next().c_str());
        else if (a == "--new-key") {
            wantNewKey = true;
            newKeyName = next();
        } else if (a == "-h" || a == "--help") {
            print_usage();
            return 0;
        } else {
            std::fprintf(stderr, "option inconnue : %s\n", a.c_str());
            print_usage();
            return 2;
        }
    }

    if (wantNewKey) return new_key(newKeyName, rate);

    if (keysFile.empty()) {
        std::fprintf(stderr,
                     "--keys est requis : sans clés, le service refuserait "
                     "tout le monde.\n");
        print_usage();
        return 2;
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_keysPath = fs::path(keysFile);
        std::error_code ec;
        if (!fs::is_regular_file(g_keysPath, ec)) {
            std::fprintf(stderr, "fichier de clés introuvable : %s\n",
                         keysFile.c_str());
            return 2;
        }
        refresh_keys_locked();
        if (g_keys.empty())
            std::fprintf(stderr,
                         "ATTENTION : aucune clé chargée, tout sera refusé.\n");
    }

    httplib::Server srv;
    srv.set_payload_max_length(maxBody);

    srv.Post("/v1/diag/crash",
             [](const httplib::Request& rq, httplib::Response& rs) {
                 handle("/v1/diag/crash", rq, rs, &tl::diagapi::crash);
             });
    srv.Post("/v1/diag/mods",
             [](const httplib::Request& rq, httplib::Response& rs) {
                 handle("/v1/diag/mods", rq, rs, &tl::diagapi::mods);
             });
    srv.Get("/v1", [](const httplib::Request& rq, httplib::Response& rs) {
        const auto a = authorise(rq);
        if (!a.ok) {
            send_json(rs, a.rateLimited ? 429 : 401,
                      R"({"error":"clé manquante, inconnue ou révoquée"})");
            return;
        }
        send_json(rs, 200, tl::diagapi::describe());
    });
    // Sans clé : une sonde de supervision ne doit pas en avoir besoin, et
    // cette route ne révèle rien.
    srv.Get("/healthz", [](const httplib::Request&, httplib::Response& rs) {
        send_json(rs, 200,
                  std::string(R"({"ok":true,"version":")") +
                      TL_VERSION_STRING + R"("})");
    });

    // Un corps trop gros est refusé par httplib avant d'arriver ici ; on
    // rend quand même du JSON, pour qu'un appelant n'ait jamais à lire du
    // HTML.
    srv.set_error_handler([](const httplib::Request&, httplib::Response& rs) {
        if (rs.body.empty())
            rs.set_content(R"({"error":"requête refusée"})",
                           "application/json");
    });
    srv.set_exception_handler(
        [](const httplib::Request& rq, httplib::Response& rs,
           std::exception_ptr) {
            // Jamais le détail de l'exception à l'appelant : il pourrait
            // porter un fragment du corps.
            rs.status = 500;
            rs.set_content(R"({"error":"erreur interne"})", "application/json");
            log_line(rq.path + " 500 (exception)");
        });

    log_line("tl_diagd " TL_VERSION_STRING " écoute sur " + host + ":" +
             std::to_string(port) + ", corps max " +
             std::to_string(maxBody / 1024) + " Ko");
    log_line("rappel : le corps des requêtes n'est jamais écrit sur disque");

    if (!srv.listen(host.c_str(), port)) {
        std::fprintf(stderr, "impossible d'écouter sur %s:%d\n", host.c_str(),
                     port);
        return 1;
    }
    return 0;
}

#pragma once
// Côté launcher du système de plugins V8 : lance tl_plugin_host, lui
// confie les plugins autorisés, et répond aux appels qu'il transmet.
//
// DÉFENSE EN PROFONDEUR : le launcher ne fait pas confiance au host non
// plus. Le host lit les octets des plugins ; s'il était compromis, il
// pourrait forger des « appels ». Chaque appel reçu est donc REVALIDÉ ici :
//   - méthode et paramètres repassent par plugin_proto::parse_request ;
//   - le plugin doit être un de ceux que CE processus a démarrés ;
//   - la permission doit figurer dans la liste tenue par le launcher, pas
//     dans celle que le host prétendrait.
// Un host compromis n'obtient ainsi jamais plus que l'union des
// permissions déjà accordées.
//
// Fil d'exécution : les appels sont mis en file par un fil de lecture et
// traités dans pump(), à appeler depuis le fil de l'interface — le même
// modèle que l'API locale. Le gestionnaire accède donc à l'état du
// launcher sans verrou supplémentaire.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::pluginhost {

struct Launch {
    std::string id;                       // nom du dossier du plugin
    std::string program;
    std::vector<std::string> args;
    std::string workDir;
    std::vector<std::string> permissions; // déjà approuvées par l'utilisateur
    std::string stderrPath;
};

struct Call {
    std::string plugin;
    std::int64_t rid = 0;
    std::string method;
    nlohmann::json params; // revalidé
};

// started, failed, exited, killed, denied, killall, log, host_died
struct Event {
    std::string plugin;
    std::string kind;
    std::string detail;
};

// Rend le résultat. Pour refuser, renseigner *errCode (≠ 0) et *errMsg.
using Handler = std::function<nlohmann::json(const Call&, int* errCode, std::string* errMsg)>;
using EventSink = std::function<void(const Event&)>;

// Exécutable du host, à côté de celui du launcher.
std::filesystem::path default_host_path();

// Lance le host et attend son message « ready » (5 s au plus).
bool start(const std::filesystem::path& hostExe, std::string* err);
void stop();
bool running();

bool start_plugin(const Launch& l, std::string* err);
void stop_plugin(const std::string& id);

// Arrêt immédiat de TOUS les plugins (kill-switch local, révocation).
void kill_all();

// Traite les appels et événements en attente. Depuis UN seul fil.
void pump(const Handler& handler, const EventSink& onEvent);

} // namespace tl::pluginhost

#pragma once

// Portage de PterodactylApi.cs (Client API) + modele d'hote persistant.
// Logique pure, sans ImGui : construction des URL, parsing des reponses,
// validation des hotes. Reseau uniquement via tl::http (WinHTTP, TLS natif).

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::ptero {

// Hote Pterodactyl : un serveur distant pilote via la Client API du panel.
// (C# : VpsUrl/VpsApiKey globaux + identifier ; ici un quadruplet par hote
// pour gerer plusieurs serveurs.)
struct Host {
    std::string name;     // nom d'affichage
    std::string panelUrl; // https://panel.exemple.fr (sans / final)
    std::string apiKey;   // cle Client API (jamais affichee ni journalisee)
    std::string serverId; // identifier court Pterodactyl (ex : "a1b2c3d4")
};

bool operator==(const Host& a, const Host& b);
bool operator!=(const Host& a, const Host& b);

// Modeles fideles a PterodactylApi.cs (PtServer/PtServerState/PtFile/PtAllocation).
struct PtServer {
    std::string id;
    std::string name;
    std::string node;
    std::string status = "unknown";
    int cpu = 0;
    long long memBytes = 0;
    long long diskBytes = 0;
    std::vector<int> allocations; // ports
};

struct PtServerState {
    bool isRunning = false;
    bool isInstalling = false;
    int cpuPercent = 0;
    long long memUsedBytes = 0;
    long long diskUsedBytes = 0;
    int uptimeSeconds = 0;
    int players = 0;
    int maxPlayers = 0;
};

struct PtFile {
    std::string name;
    bool isDirectory = false;
    long long size = 0;
    std::string mimeType;
    std::string modified; // ISO-8601 tel que renvoye (C# : DateTime)
};

struct PtAllocation {
    std::string ip;
    int port = 0;
};

// --- Validation (aucun reseau) ---
// false + message FR si : nom vide, URL sans scheme http(s) ou sans hote,
// cle vide, identifiant vide/trop long ou hors [A-Za-z0-9_-]
// (garde anti-injection de segment de chemin).
bool valid_host(const Host& h, std::string* errorFr = nullptr);

// --- Construction d'URL (aucun reseau) ---
// panelUrl (sans / final) + "/api/client" + path.
std::string api_url(const Host& h, const std::string& path);
// "Authorization: Bearer <cle>\r\nAccept: application/json" (format WinHTTP).
std::string auth_headers(const Host& h);

// --- (De)serialisation JSON (cles PascalCase, comme le reste du config) ---
nlohmann::json host_to_json(const Host& h);
Host host_from_json(const nlohmann::json& j); // leve si champ manquant/invalide

// --- Persistance (DataStore::settings.pteroHosts, cle "PteroHosts") ---
std::vector<Host> load_hosts();                  // ignore les entrees invalides
void save_hosts(const std::vector<Host>& hosts); // ecrit + DataStore::save()
void add_host(const Host& h); // valide (leve sinon) ; remplace a (url,id) egaux
bool remove_host(const std::string& panelUrl, const std::string& serverId);

// --- Parsing des reponses (aucun reseau ; leve si JSON illisible) ---
std::vector<PtServer> parse_server_list(const std::string& body);
// Etat par defaut (arrete) si "attributes" absent, comme le C#.
PtServerState parse_server_state(const std::string& body);
std::optional<std::string> parse_websocket_token(const std::string& body);
std::vector<PtFile> parse_file_list(const std::string& body);
std::vector<PtAllocation> parse_allocations(const std::string& body);

// --- Client API (leve en cas d'echec reseau/HTTP ; messages en francais) ---
std::vector<PtServer> list_servers(const Host& h,
                                   const std::atomic<bool>* cancel = nullptr);
// Fidele au C# (GetServerStateAsync) : ne leve jamais pour un probleme
// reseau/HTTP, rend l'etat par defaut (arrete). Leve seulement si le corps
// recu n'est pas du JSON parsable... comme le C# qui attrape tout, on
// prefere rendre l'etat par defaut dans tous les cas (noexcept).
PtServerState server_state(const Host& h,
                           const std::atomic<bool>* cancel = nullptr) noexcept;
// nullopt en cas d'echec (C# : null).
std::optional<std::string> websocket_token(const Host& h,
                                           const std::atomic<bool>* cancel = nullptr);
// signal : start|stop|restart|kill (tout autre -> invalid_argument, sans reseau).
void power(const Host& h, const std::string& signal,
           const std::atomic<bool>* cancel = nullptr);
void send_command(const Host& h, const std::string& command,
                  const std::atomic<bool>* cancel = nullptr);
std::vector<PtFile> list_files(const Host& h, const std::string& directory = "/",
                               const std::atomic<bool>* cancel = nullptr);
void delete_files(const Host& h, const std::string& root,
                  const std::vector<std::string>& files,
                  const std::atomic<bool>* cancel = nullptr);
std::vector<PtAllocation> allocations(const Host& h,
                                      const std::atomic<bool>* cancel = nullptr);

namespace detail {

std::string trim(const std::string& s);
std::string url_encode(const std::string& s); // %XX (cle : comme modrinth)

} // namespace detail

} // namespace tl::ptero

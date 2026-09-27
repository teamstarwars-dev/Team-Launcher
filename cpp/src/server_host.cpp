#include "server_host.hpp"

#include "datastore.hpp"
#include "http_win.hpp"

#include <cctype>
#include <stdexcept>

using nlohmann::json;

namespace tl::ptero {

bool operator==(const Host& a, const Host& b) {
    return a.name == b.name && a.panelUrl == b.panelUrl &&
           a.apiKey == b.apiKey && a.serverId == b.serverId;
}

bool operator!=(const Host& a, const Host& b) { return !(a == b); }

namespace detail {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out.push_back(static_cast<char>(ch));
        else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0xF]);
        }
    }
    return out;
}

} // namespace detail

namespace {

std::string no_trailing_slash(const std::string& url) {
    std::string u = detail::trim(url);
    while (u.size() > 1 && u.back() == '/') u.pop_back();
    return u;
}

// Leve runtime_error (message FR) si l'hote est invalide.
void ensure_valid(const Host& h) {
    std::string err;
    if (!valid_host(h, &err)) throw std::runtime_error(err);
}

int json_int(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number_integer()) return static_cast<int>(it->get<long long>());
    if (it->is_number_unsigned()) return static_cast<int>(it->get<unsigned long long>());
    if (it->is_number_float()) return static_cast<int>(it->get<double>());
    return 0;
}

long long json_ll(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_number_unsigned()) return static_cast<long long>(it->get<unsigned long long>());
    if (it->is_number_float()) return static_cast<long long>(it->get<double>());
    return 0;
}

double json_double(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return 0.0;
    if (it->is_number()) return it->get<double>();
    return 0.0;
}

std::string json_str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

json parse_body(const std::string& body) {
    try {
        return json::parse(body);
    } catch (const json::exception&) {
        throw std::runtime_error("Réponse Pterodactyl illisible.");
    }
}

// GET /api/client<path> : corps brut. Leve si echec (comme EnsureSuccessStatusCode).
std::string http_get(const Host& h, const std::string& path,
                     const std::atomic<bool>* cancel) {
    ensure_valid(h);
    auto r = http::get_response(api_url(h, path), auth_headers(h), cancel);
    if (!r) throw std::runtime_error("Échec réseau vers le panel Pterodactyl.");
    if (r->status < 200 || r->status > 299)
        throw std::runtime_error("HTTP " + std::to_string(r->status) +
                                 " depuis le panel Pterodactyl.");
    return r->body;
}

// POST /api/client<path> + corps JSON : ignore le corps de reponse
// (le C# accepte vide/"null" ; ici 2xx suffit, comme EnsureSuccessStatusCode).
void http_post(const Host& h, const std::string& path, const std::string& body,
               const std::atomic<bool>* cancel) {
    ensure_valid(h);
    auto r = http::post_string(api_url(h, path), body, "application/json",
                               auth_headers(h), cancel);
    if (!r) throw std::runtime_error("Échec réseau vers le panel Pterodactyl.");
    if (r->status < 200 || r->status > 299)
        throw std::runtime_error("HTTP " + std::to_string(r->status) +
                                 " depuis le panel Pterodactyl.");
}

} // namespace

// ---------------------------------------------------------------------------
// Validation / URL
// ---------------------------------------------------------------------------

bool valid_host(const Host& h, std::string* errorFr) {
    auto fail = [&](const char* m) {
        if (errorFr) *errorFr = m;
        return false;
    };
    if (detail::trim(h.name).empty())
        return fail("Le nom de l'hôte est vide.");
    const std::string url = detail::trim(h.panelUrl);
    const bool https = url.rfind("https://", 0) == 0;
    const bool http = url.rfind("http://", 0) == 0;
    const size_t schemeLen = https ? 8 : (http ? 7 : 0);
    if (schemeLen == 0 || url.size() <= schemeLen ||
        url.find(' ') != std::string::npos || url.find('\\') != std::string::npos)
        return fail("L'URL du panel doit commencer par http:// ou https://.");
    if (detail::trim(h.apiKey).empty())
        return fail("La clé API est vide.");
    if (h.serverId.empty() || h.serverId.size() > 64)
        return fail("L'identifiant serveur est vide.");
    for (char c : h.serverId) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!std::isalnum(u) && c != '_' && c != '-')
            return fail("L'identifiant serveur contient des caractères invalides.");
    }
    return true;
}

std::string api_url(const Host& h, const std::string& path) {
    std::string url = no_trailing_slash(h.panelUrl) + "/api/client";
    if (path.empty() || path[0] != '/') url.push_back('/');
    url += path;
    return url;
}

std::string auth_headers(const Host& h) {
    return "Authorization: Bearer " + h.apiKey + "\r\nAccept: application/json";
}

// ---------------------------------------------------------------------------
// (De)serialisation / persistance
// ---------------------------------------------------------------------------

json host_to_json(const Host& h) {
    return json{{"Name", h.name},
                {"PanelUrl", h.panelUrl},
                {"ApiKey", h.apiKey},
                {"ServerId", h.serverId}};
}

Host host_from_json(const json& j) {
    if (!j.is_object()) throw std::runtime_error("Hôte Pterodactyl illisible.");
    Host h;
    h.name = json_str(j, "Name");
    h.panelUrl = json_str(j, "PanelUrl");
    h.apiKey = json_str(j, "ApiKey");
    h.serverId = json_str(j, "ServerId");
    std::string err;
    if (!valid_host(h, &err)) throw std::runtime_error(err);
    return h;
}

std::vector<Host> load_hosts() {
    std::vector<Host> out;
    const json& arr = DataStore::settings.pteroHosts;
    if (!arr.is_array()) return out;
    for (const auto& e : arr) {
        try {
            out.push_back(host_from_json(e));
        } catch (...) {
            // entree corrompue : ignoree (les autres sont conservees)
        }
    }
    return out;
}

void save_hosts(const std::vector<Host>& hosts) {
    json arr = json::array();
    for (const auto& h : hosts) arr.push_back(host_to_json(h));
    DataStore::settings.pteroHosts = std::move(arr);
    DataStore::save();
}

void add_host(const Host& h) {
    Host norm = h;
    norm.name = detail::trim(norm.name);
    norm.panelUrl = no_trailing_slash(norm.panelUrl);
    std::string err;
    if (!valid_host(norm, &err)) throw std::runtime_error(err);
    auto hosts = load_hosts();
    bool replaced = false;
    for (auto& e : hosts)
        if (e.panelUrl == norm.panelUrl && e.serverId == norm.serverId) {
            e = norm;
            replaced = true;
        }
    if (!replaced) hosts.push_back(norm);
    save_hosts(hosts);
}

bool remove_host(const std::string& panelUrl, const std::string& serverId) {
    const std::string url = no_trailing_slash(panelUrl);
    auto hosts = load_hosts();
    const size_t before = hosts.size();
    for (auto it = hosts.begin(); it != hosts.end();) {
        if (it->panelUrl == url && it->serverId == serverId)
            it = hosts.erase(it);
        else
            ++it;
    }
    if (hosts.size() == before) return false;
    save_hosts(hosts);
    return true;
}

// ---------------------------------------------------------------------------
// Parsing
// ---------------------------------------------------------------------------

std::vector<PtServer> parse_server_list(const std::string& body) {
    const json root = parse_body(body);
    if (!root.is_object()) throw std::runtime_error("Réponse Pterodactyl illisible.");
    std::vector<PtServer> out;
    auto data = root.find("data");
    if (data == root.end() || !data->is_array()) return out;
    for (const auto& s : *data) {
        if (!s.is_object()) continue;
        auto attr = s.find("attributes");
        if (attr == s.end() || !attr->is_object()) continue;
        PtServer e;
        e.id = json_str(*attr, "identifier");
        e.name = json_str(*attr, "name");
        e.node = json_str(*attr, "node");
        e.status = json_str(*attr, "status");
        if (e.status.empty()) e.status = "unknown";
        e.cpu = json_int(*attr, "cpu");
        e.memBytes = json_ll(*attr, "memory");
        e.diskBytes = json_ll(*attr, "disk");
        auto al = attr->find("allocations");
        if (al != attr->end() && al->is_array())
            for (const auto& a : *al) {
                if (!a.is_object()) continue;
                auto p = a.find("port");
                if (p != a.end() && p->is_number())
                    e.allocations.push_back(json_int(a, "port"));
            }
        out.push_back(std::move(e));
    }
    return out;
}

PtServerState parse_server_state(const std::string& body) {
    const json root = parse_body(body);
    PtServerState st;
    if (!root.is_object()) return st;
    auto attr = root.find("attributes");
    if (attr == root.end() || !attr->is_object()) return st; // comme le C#
    auto running = attr->find("running");
    st.isRunning = running != attr->end() && running->is_boolean() && running->get<bool>();
    auto installing = attr->find("installing");
    st.isInstalling =
        installing != attr->end() && installing->is_boolean() && installing->get<bool>();
    st.cpuPercent = static_cast<int>(json_double(*attr, "cpu_absolute")); // (int)GetDouble
    st.memUsedBytes = json_ll(*attr, "memory_bytes");
    st.diskUsedBytes = json_ll(*attr, "disk_bytes");
    st.uptimeSeconds = json_int(*attr, "uptime");
    auto state = attr->find("state");
    if (state != attr->end() && state->is_object()) {
        st.players = json_int(*state, "players");
        st.maxPlayers = json_int(*state, "max_players");
    }
    return st;
}

std::optional<std::string> parse_websocket_token(const std::string& body) {
    const json root = parse_body(body);
    if (!root.is_object()) return std::nullopt;
    auto data = root.find("data");
    if (data == root.end() || !data->is_object()) return std::nullopt;
    auto tok = data->find("token");
    if (tok == data->end() || !tok->is_string()) return std::nullopt;
    return tok->get<std::string>();
}

std::vector<PtFile> parse_file_list(const std::string& body) {
    const json root = parse_body(body);
    if (!root.is_object()) throw std::runtime_error("Réponse Pterodactyl illisible.");
    std::vector<PtFile> out;
    auto data = root.find("data");
    if (data == root.end() || !data->is_array()) return out;
    for (const auto& f : *data) {
        if (!f.is_object()) continue;
        auto attr = f.find("attributes");
        if (attr == f.end() || !attr->is_object()) continue;
        PtFile e;
        e.name = json_str(*attr, "name");
        auto isFile = attr->find("is_file");
        // C# : isDir = (is_file == false)
        e.isDirectory = !(isFile != attr->end() && isFile->is_boolean() && isFile->get<bool>());
        e.size = json_ll(*attr, "size");
        e.mimeType = json_str(*attr, "mime_type");
        e.modified = json_str(*attr, "modified_at");
        out.push_back(std::move(e));
    }
    return out;
}

std::vector<PtAllocation> parse_allocations(const std::string& body) {
    const json root = parse_body(body);
    if (!root.is_object()) throw std::runtime_error("Réponse Pterodactyl illisible.");
    std::vector<PtAllocation> out;
    auto data = root.find("data");
    if (data == root.end() || !data->is_array()) return out;
    for (const auto& a : *data) {
        if (!a.is_object()) continue;
        auto attr = a.find("attributes");
        if (attr == a.end() || !attr->is_object()) continue;
        PtAllocation e;
        e.ip = json_str(*attr, "ip");
        e.port = json_int(*attr, "port");
        out.push_back(std::move(e));
    }
    return out;
}

// ---------------------------------------------------------------------------
// Client API
// ---------------------------------------------------------------------------

std::vector<PtServer> list_servers(const Host& h, const std::atomic<bool>* cancel) {
    return parse_server_list(http_get(h, "/servers", cancel));
}

PtServerState server_state(const Host& h, const std::atomic<bool>* cancel) noexcept {
    try {
        return parse_server_state(
            http_get(h, "/servers/" + h.serverId + "/resources", cancel));
    } catch (...) {
        return PtServerState{}; // fidele au C# : catch { } -> etat par defaut
    }
}

std::optional<std::string> websocket_token(const Host& h,
                                           const std::atomic<bool>* cancel) {
    try {
        return parse_websocket_token(
            http_get(h, "/servers/" + h.serverId + "/websocket", cancel));
    } catch (...) {
        return std::nullopt; // fidele au C# : catch { } -> null
    }
}

void power(const Host& h, const std::string& signal,
           const std::atomic<bool>* cancel) {
    if (signal != "start" && signal != "stop" && signal != "restart" && signal != "kill")
        throw std::invalid_argument("Signal d'alimentation inconnu : " + signal + ".");
    ensure_valid(h);
    const json body = {{"signal", signal}};
    http_post(h, "/servers/" + h.serverId + "/power", body.dump(), cancel);
}

void send_command(const Host& h, const std::string& command,
                  const std::atomic<bool>* cancel) {
    ensure_valid(h);
    const json body = {{"command", command}};
    http_post(h, "/servers/" + h.serverId + "/command", body.dump(), cancel);
}

std::vector<PtFile> list_files(const Host& h, const std::string& directory,
                               const std::atomic<bool>* cancel) {
    return parse_file_list(http_get(h, "/servers/" + h.serverId + "/files/list?directory=" +
                                           detail::url_encode(directory),
                                   cancel));
}

void delete_files(const Host& h, const std::string& root,
                  const std::vector<std::string>& files,
                  const std::atomic<bool>* cancel) {
    ensure_valid(h);
    const json body = {{"root", root}, {"files", files}};
    http_post(h, "/servers/" + h.serverId + "/files/delete", body.dump(), cancel);
}

std::vector<PtAllocation> allocations(const Host& h, const std::atomic<bool>* cancel) {
    return parse_allocations(http_get(h, "/servers/" + h.serverId + "/network/allocations",
                                      cancel));
}

} // namespace tl::ptero

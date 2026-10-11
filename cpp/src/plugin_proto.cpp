#include "plugin_proto.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <set>

using nlohmann::json;

namespace tl::pluginproto {

// --- Trames ---------------------------------------------------------------

std::string encode_frame(const std::string& payload) {
    const auto n = static_cast<std::uint32_t>(payload.size());
    std::string out;
    out.reserve(4 + payload.size());
    out.push_back(static_cast<char>(n & 0xFF));
    out.push_back(static_cast<char>((n >> 8) & 0xFF));
    out.push_back(static_cast<char>((n >> 16) & 0xFF));
    out.push_back(static_cast<char>((n >> 24) & 0xFF));
    out += payload;
    return out;
}

void FrameReader::feed(const char* data, std::size_t n) {
    if (poisoned_ || n == 0) return;
    // Borne du tampon : au plus une trame complète plus son en-tête en
    // attente. Un émetteur qui inonde sans jamais finir une trame ne peut
    // pas faire grossir la mémoire du host indéfiniment.
    if (buf_.size() + n > kMaxFrame + 4 + kMaxFrame) {
        poisoned_ = true;
        buf_.clear();
        return;
    }
    buf_.append(data, n);
}

FrameReader::Status FrameReader::next(std::string* out) {
    if (poisoned_) return Status::Invalid;
    if (buf_.size() < 4) return Status::NeedMore;
    const auto b = reinterpret_cast<const unsigned char*>(buf_.data());
    const std::uint32_t len = static_cast<std::uint32_t>(b[0]) |
                              (static_cast<std::uint32_t>(b[1]) << 8) |
                              (static_cast<std::uint32_t>(b[2]) << 16) |
                              (static_cast<std::uint32_t>(b[3]) << 24);
    if (len == 0 || len > kMaxFrame) {
        poisoned_ = true;
        buf_.clear();
        return Status::Invalid;
    }
    if (buf_.size() < 4 + static_cast<std::size_t>(len)) return Status::NeedMore;
    out->assign(buf_, 4, len);
    buf_.erase(0, 4 + static_cast<std::size_t>(len));
    return Status::Frame;
}

// --- Permissions ----------------------------------------------------------

const std::vector<std::string>& known_permissions() {
    // Liste VOLONTAIREMENT courte. Chaque entrée est une capacité qu'un
    // plugin malveillant pourrait retourner contre l'utilisateur ; on en
    // ajoute quand un besoin réel le justifie, pas par anticipation.
    static const std::vector<std::string> k = {
        "instances:read", // lire la liste et le détail des instances
        "mods:read",      // lire la liste des mods d'une instance
        "ui:notify",      // afficher une notification dans le launcher
        "game:launch",    // lancer une instance — la plus sensible
    };
    return k;
}

bool is_known_permission(const std::string& p) {
    const auto& k = known_permissions();
    return std::find(k.begin(), k.end(), p) != k.end();
}

std::string permission_label(const std::string& p) {
    if (p == "instances:read") return "Lire vos instances (noms, versions, chargeurs)";
    if (p == "mods:read") return "Lire la liste des mods de vos instances";
    if (p == "ui:notify") return "Afficher des notifications dans le launcher";
    if (p == "game:launch") return "Lancer le jeu sur une de vos instances";
    return "Permission inconnue : " + p;
}

// --- Validation -----------------------------------------------------------

int json_depth(const std::string& s) {
    int depth = 0, maxd = 0;
    bool inStr = false, esc = false;
    for (const char c : s) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[' || c == '{') {
            if (++depth > maxd) maxd = depth;
            if (maxd > kMaxDepth) return kMaxDepth + 1;
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return maxd;
}

bool is_instance_id(const std::string& s) {
    if (s.size() != 32) return false;
    for (const char c : s)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

namespace {

// Chaîne de longueur bornée, sans caractère de contrôle hormis \t et \n.
// Les caractères de contrôle n'ont rien à faire dans un message de
// plugin, et certains (\r, \x1b) servent à maquiller un journal ou une
// notification.
bool clean_string(const json& v, std::size_t maxLen) {
    if (!v.is_string()) return false;
    const auto& s = v.get_ref<const std::string&>();
    if (s.empty() || s.size() > maxLen) return false;
    for (const unsigned char c : s)
        if (c < 0x20 && c != '\t' && c != '\n') return false;
    return true;
}

// Les clés de `o` sont-elles toutes dans `allowed`, et les obligatoires
// toutes présentes ?
bool exact_keys(const json& o, std::initializer_list<const char*> required,
                std::initializer_list<const char*> optional = {}) {
    if (!o.is_object()) return false;
    for (const char* k : required)
        if (!o.contains(k)) return false;
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool ok = false;
        for (const char* k : required) ok = ok || it.key() == k;
        for (const char* k : optional) ok = ok || it.key() == k;
        if (!ok) return false;
    }
    return true;
}

struct Method {
    const char* name;
    const char* permission; // "" = toujours permise
    bool (*validate)(const json& params);
};

bool v_empty(const json& p) { return p.is_object() && p.empty(); }

bool v_hello(const json& p) {
    if (!exact_keys(p, {"api"})) return false;
    return p["api"].is_number_integer() && p["api"].get<std::int64_t>() >= 1 &&
           p["api"].get<std::int64_t>() <= 1000;
}

bool v_log(const json& p) {
    if (!exact_keys(p, {"level", "message"})) return false;
    if (!p["level"].is_string()) return false;
    const auto& l = p["level"].get_ref<const std::string&>();
    return (l == "info" || l == "warn" || l == "error") &&
           clean_string(p["message"], 2000);
}

bool v_instance(const json& p) {
    if (!exact_keys(p, {"instanceId"})) return false;
    return p["instanceId"].is_string() &&
           is_instance_id(p["instanceId"].get_ref<const std::string&>());
}

bool v_notify(const json& p) {
    if (!exact_keys(p, {"title", "message"})) return false;
    return clean_string(p["title"], 80) && clean_string(p["message"], 500);
}

const Method kMethods[] = {
    {"host.hello", "", v_hello},
    {"log.write", "", v_log},
    {"instances.list", "instances:read", v_empty},
    {"instances.get", "instances:read", v_instance},
    {"mods.list", "mods:read", v_instance},
    {"ui.notify", "ui:notify", v_notify},
    {"game.launch", "game:launch", v_instance},
};

const Method* find(const std::string& name) {
    for (const auto& m : kMethods)
        if (name == m.name) return &m;
    return nullptr;
}

} // namespace

std::optional<std::string> required_permission(const std::string& method) {
    const Method* m = find(method);
    if (!m) return std::nullopt;
    return std::string(m->permission);
}

std::optional<Request> parse_request(const std::string& frame, Error* err) {
    auto fail = [&](int code, const char* msg, std::int64_t id = -1) {
        if (err) *err = Error{code, msg, id};
        return std::nullopt;
    };

    if (json_depth(frame) > kMaxDepth)
        return fail(kParseError, "imbrication excessive");

    json j = json::parse(frame, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded()) return fail(kParseError, "JSON invalide");

    // UTF-8 : nlohmann n'échoue sur des octets invalides qu'à la
    // SÉRIALISATION. On la tente ici, pour refuser à l'entrée ce qu'on ne
    // pourrait de toute façon pas renvoyer ni journaliser.
    try {
        (void)j.dump();
    } catch (const json::exception&) {
        return fail(kParseError, "UTF-8 invalide");
    }

    if (!j.is_object()) return fail(kInvalidRequest, "la requête doit être un objet");

    // L'id d'abord : sans lui, on ne peut même pas adresser l'erreur.
    std::int64_t id = -1;
    if (auto it = j.find("id"); it != j.end() && it->is_number_integer()) {
        const auto v = it->get<std::int64_t>();
        if (v >= 0 && v <= 0x7FFFFFFF) id = v;
    }
    if (id < 0) return fail(kInvalidRequest, "id manquant ou hors limites");

    if (!exact_keys(j, {"v", "id", "method"}, {"params"}))
        return fail(kInvalidRequest, "clés inattendues ou manquantes", id);
    if (!j["v"].is_number_integer() || j["v"].get<std::int64_t>() != kApiVersion)
        return fail(kInvalidRequest, "version de protocole non prise en charge", id);
    if (!j["method"].is_string())
        return fail(kInvalidRequest, "method doit être une chaîne", id);

    const std::string method = j["method"].get<std::string>();
    const Method* m = find(method);
    if (!m) return fail(kMethodNotFound, "méthode inconnue", id);

    json params = j.contains("params") ? j["params"] : json::object();
    if (!params.is_object() || !m->validate(params))
        return fail(kInvalidParams, "paramètres invalides", id);

    return Request{id, method, std::move(params)};
}

std::string make_result(std::int64_t id, const json& result) {
    return json{{"v", kApiVersion}, {"id", id}, {"result", result}}.dump(
        -1, ' ', false, json::error_handler_t::replace);
}

std::string make_error(const Error& e) {
    json j{{"v", kApiVersion},
           {"error", {{"code", e.code}, {"message", e.message}}}};
    if (e.id >= 0) j["id"] = e.id;
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

} // namespace tl::pluginproto

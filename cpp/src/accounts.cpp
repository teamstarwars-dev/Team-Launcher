#include "accounts.hpp"

#include "datastore.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <chrono>
#include <fstream>
#include <mutex>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::accounts {

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Les deux fichiers qui composent une session. `ms_auth.cpp` les nomme de
// la meme facon ; ils sont redefinis ici plutot qu'exposes, pour ne pas
// elargir l'API publique de l'authentification a des chemins internes.
fs::path session_file() { return DataStore::dir() / "session-cache.json"; }
fs::path refresh_file() { return DataStore::dir() / "msauth.json"; }
fs::path store_file() { return DataStore::dir() / "accounts.json"; }

std::optional<std::string> read_all(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::string s((std::istreambuf_iterator<char>(in)),
                  std::istreambuf_iterator<char>());
    if (s.empty()) return std::nullopt;
    return s;
}

bool write_atomic(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".part";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return false;
        }
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

json load_store() {
    auto raw = read_all(store_file());
    if (!raw) return json::object();
    auto j = json::parse(*raw, nullptr, false);
    // Un magasin illisible ne doit pas empecher de se connecter : on
    // repart d'un magasin vide plutot que de refuser le demarrage.
    if (j.is_discarded() || !j.is_object()) return json::object();
    if (!j.contains("accounts") || !j["accounts"].is_array())
        j["accounts"] = json::array();
    return j;
}

void save_store(const json& j) { write_atomic(store_file(), j.dump(1)); }

// Nom et UUID du compte actif, lus dans le cache de session. Ce sont les
// deux seuls champs EN CLAIR du fichier ; le jeton, lui, reste chiffre et
// n'est jamais touche ici.
bool current_identity(std::string& uuid, std::string& name) {
    auto raw = read_all(session_file());
    if (!raw) return false;
    const auto j = json::parse(*raw, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;
    uuid = j.value("uuid", "");
    name = j.value("name", "");
    return !uuid.empty();
}

} // namespace

std::string store_path() { return store_file().string(); }

std::string current_uuid() {
    std::lock_guard<std::mutex> lk(lock());
    std::string u, n;
    return current_identity(u, n) ? u : std::string();
}

std::vector<Account> list() {
    std::lock_guard<std::mutex> lk(lock());
    const json j = load_store();
    std::vector<Account> out;
    for (const auto& a : j["accounts"]) {
        if (!a.is_object()) continue;
        Account x;
        x.uuid = a.value("uuid", "");
        x.name = a.value("name", "");
        if (x.uuid.empty()) continue;
        x.savedAt = a.value("savedAt", 0LL);
        x.hasRefresh = !a.value("refresh", std::string()).empty();
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Account& a, const Account& b) {
        return a.savedAt > b.savedAt;
    });
    return out;
}

void remember_current() {
    std::lock_guard<std::mutex> lk(lock());
    std::string uuid, name;
    if (!current_identity(uuid, name)) return;

    const auto sess = read_all(session_file());
    if (!sess) return; // rien a retenir
    const auto refresh = read_all(refresh_file());

    json j = load_store();
    json& arr = j["accounts"];
    json entry = {{"uuid", uuid},
                  {"name", name},
                  {"savedAt", now_unix()},
                  {"session", *sess}};
    // Le jeton de rafraichissement peut manquer (plateforme sans coffre) :
    // le compte reste listable, il faudra juste se reconnecter en basculant.
    if (refresh) entry["refresh"] = *refresh;

    bool replaced = false;
    for (auto& a : arr)
        if (a.is_object() && a.value("uuid", "") == uuid) {
            // L'interface appelle cette fonction A CHAQUE FRAME tant qu'une
            // session est active. Sans cette comparaison, accounts.json
            // serait reecrit soixante fois par seconde — inutile, et
            // mauvais pour un SSD. On ne touche au disque que si le jeton
            // ou le nom ont reellement change.
            if (a.value("session", std::string()) ==
                    entry.value("session", std::string()) &&
                a.value("refresh", std::string()) ==
                    entry.value("refresh", std::string()) &&
                a.value("name", std::string()) ==
                    entry.value("name", std::string()))
                return;
            a = entry;
            replaced = true;
            break;
        }
    if (!replaced) arr.push_back(entry);
    save_store(j);
}

bool switch_to(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(lock());
    if (uuid.empty()) return false;

    // On enregistre d'abord la session courante, sinon basculer la perdrait.
    {
        std::string cu, cn;
        if (current_identity(cu, cn) && cu != uuid) {
            if (auto sess = read_all(session_file())) {
                const auto refresh = read_all(refresh_file());
                json j = load_store();
                json& arr = j["accounts"];
                json entry = {{"uuid", cu},
                              {"name", cn},
                              {"savedAt", now_unix()},
                              {"session", *sess}};
                if (refresh) entry["refresh"] = *refresh;
                bool replaced = false;
                for (auto& a : arr)
                    if (a.is_object() && a.value("uuid", "") == cu) {
                        a = entry;
                        replaced = true;
                        break;
                    }
                if (!replaced) arr.push_back(entry);
                save_store(j);
            }
        }
    }

    json j = load_store();
    for (auto& a : j["accounts"]) {
        if (!a.is_object() || a.value("uuid", "") != uuid) continue;
        const std::string sess = a.value("session", "");
        if (sess.empty()) return false;
        if (!write_atomic(session_file(), sess)) return false;
        const std::string refresh = a.value("refresh", "");
        std::error_code ec;
        if (refresh.empty())
            // Pas de jeton de rafraichissement pour ce compte : on retire
            // celui de l'ancien, sinon le renouvellement silencieux
            // rafraichirait le MAUVAIS compte.
            fs::remove(refresh_file(), ec);
        else
            write_atomic(refresh_file(), refresh);
        a["savedAt"] = now_unix(); // remonte en tete de liste
        save_store(j);
        return true;
    }
    return false;
}

bool forget(const std::string& uuid) {
    std::lock_guard<std::mutex> lk(lock());
    if (uuid.empty()) return false;
    json j = load_store();
    json& arr = j["accounts"];
    const std::size_t before = arr.size();
    json kept = json::array();
    for (const auto& a : arr)
        if (!(a.is_object() && a.value("uuid", "") == uuid)) kept.push_back(a);
    if (kept.size() == before) return false;
    j["accounts"] = kept;
    save_store(j);

    // Si c'etait le compte actif, la session en cours part avec lui :
    // rester connecte a un compte qu'on vient d'oublier n'a pas de sens.
    std::string cu, cn;
    if (current_identity(cu, cn) && cu == uuid) {
        std::error_code ec;
        fs::remove(session_file(), ec);
        fs::remove(refresh_file(), ec);
    }
    return true;
}

} // namespace tl::accounts

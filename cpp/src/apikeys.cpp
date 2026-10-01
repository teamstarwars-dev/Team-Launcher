#include "apikeys.hpp"

#include "datastore.hpp" // new_guid (aléatoire du système), settings
#include "util_hash.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>

namespace tl::apikeys {

const char* const kScopeDiag = "diag";
const char* const kScopeRead = "read";
const char* const kScopeControl = "control";

namespace {

std::mutex& lock() {
    static std::mutex m;
    return m;
}

long long now_unix() {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
}

// Les compteurs d'usage changent à chaque appel. Les écrire à chaque fois
// ferait réécrire config.json plusieurs fois par seconde sous charge —
// exactement le défaut déjà corrigé une fois sur le store de comptes. On
// marque, et `flush()` écrit.
bool g_dirty = false;

std::vector<Key>& cache() {
    static std::vector<Key> keys;
    static bool loaded = false;
    if (!loaded) {
        loaded = true;
        keys = from_json(DataStore::settings.apiKeys);
    }
    return keys;
}

void save_locked() {
    DataStore::settings.apiKeys = to_json(cache());
    DataStore::save();
    g_dirty = false;
}

} // namespace

bool is_known_scope(const std::string& s) {
    return s == kScopeDiag || s == kScopeRead || s == kScopeControl;
}

bool Key::has(const std::string& scope) const {
    if (revoked) return false;
    if (scope.empty()) return true;
    return std::find(scopes.begin(), scopes.end(), scope) != scopes.end();
}

nlohmann::json to_json(const std::vector<Key>& keys) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& k : keys)
        arr.push_back({
            {"id", k.id},
            {"hash", k.hash},
            {"appName", k.appName},
            {"contact", k.contact},
            {"scopes", k.scopes},
            {"createdUnix", k.createdUnix},
            {"lastUsedUnix", k.lastUsedUnix},
            {"calls", k.calls},
            {"revoked", k.revoked},
        });
    return arr;
}

std::vector<Key> from_json(const nlohmann::json& j) {
    std::vector<Key> out;
    if (!j.is_array()) return out;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        Key k;
        k.id = e.value("id", std::string{});
        k.hash = e.value("hash", std::string{});
        k.appName = e.value("appName", std::string{});
        k.contact = e.value("contact", std::string{});
        if (auto s = e.find("scopes"); s != e.end() && s->is_array())
            for (const auto& v : *s)
                if (v.is_string() && is_known_scope(v.get<std::string>()))
                    k.scopes.push_back(v.get<std::string>());
        k.createdUnix = e.value("createdUnix", 0LL);
        k.lastUsedUnix = e.value("lastUsedUnix", 0LL);
        k.calls = e.value("calls", 0LL);
        k.revoked = e.value("revoked", false);
        // Une entrée sans empreinte n'authentifierait personne : la garder
        // ne ferait qu'encombrer la liste.
        if (k.id.empty() || k.hash.empty()) continue;
        out.push_back(std::move(k));
    }
    return out;
}

Created create(const std::string& appName, const std::string& contact,
               const std::vector<std::string>& scopes) {
    Created c;
    // 256 bits du générateur du système (BCryptGenRandom / getrandom).
    c.secret = new_guid() + new_guid();
    c.key.id = "ak_" + new_guid().substr(0, 12);
    c.key.hash = sha1_hex_of(c.secret);
    c.key.appName = appName.empty() ? "Application sans nom" : appName;
    c.key.contact = contact;
    for (const auto& s : scopes)
        if (is_known_scope(s) &&
            std::find(c.key.scopes.begin(), c.key.scopes.end(), s) ==
                c.key.scopes.end())
            c.key.scopes.push_back(s);
    // Aucune portée demandée : on donne la plus inoffensive plutôt que
    // rien. Une clé sans portée serait une clé qui ne sert à rien.
    if (c.key.scopes.empty()) c.key.scopes.push_back(kScopeDiag);
    c.key.createdUnix = now_unix();

    std::lock_guard<std::mutex> lk(lock());
    cache().push_back(c.key);
    save_locked();
    return c;
}

std::vector<Key> list() {
    std::lock_guard<std::mutex> lk(lock());
    return cache();
}

bool revoke(const std::string& id) {
    std::lock_guard<std::mutex> lk(lock());
    for (auto& k : cache())
        if (k.id == id && !k.revoked) {
            k.revoked = true;
            save_locked();
            return true;
        }
    return false;
}

bool remove(const std::string& id) {
    std::lock_guard<std::mutex> lk(lock());
    auto& v = cache();
    const auto before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const Key& k) { return k.id == id; }),
            v.end());
    if (v.size() == before) return false;
    save_locked();
    return true;
}

Match check(const std::string& secret, const std::string& scope) {
    Match m;
    if (secret.empty()) return m;
    const std::string h = sha1_hex_of(secret);

    std::lock_guard<std::mutex> lk(lock());
    for (auto& k : cache()) {
        // Comparaison à durée constante : les empreintes font toutes la
        // même longueur, et un `==` s'arrêtant au premier octet différent
        // laisserait mesurer combien de caractères sont bons.
        if (k.hash.size() != h.size()) continue;
        unsigned char diff = 0;
        for (size_t i = 0; i < h.size(); ++i)
            diff |= static_cast<unsigned char>(k.hash[i] ^ h[i]);
        if (diff != 0) continue;

        if (k.revoked) return m; // trouvée mais révoquée : refus net
        if (!k.has(scope)) {
            m.scopeDenied = true;
            m.id = k.id;
            m.appName = k.appName;
            return m;
        }
        k.lastUsedUnix = now_unix();
        ++k.calls;
        g_dirty = true;
        m.ok = true;
        m.id = k.id;
        m.appName = k.appName;
        return m;
    }
    return m;
}

void flush() {
    std::lock_guard<std::mutex> lk(lock());
    if (g_dirty) save_locked();
}

} // namespace tl::apikeys

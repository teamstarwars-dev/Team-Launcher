#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>
#else
// Etape 5 (Linux) : libsecret (trousseau) prioritaire, repli fichier 0600.
// Décision 28/09/2026 : jamais de stockage silencieux en clair — le repli
// lève insecure_fallback_active() pour avertir dans l'interface.
#include <libsecret/secret.h>

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex> // call_once (schéma libsecret)
#include <random>
#include <sys/random.h> // getrandom (uuid de clé)
#include <sys/stat.h>   // chmod, stat (0600)
#include <unistd.h>
#endif

#include "secrets.hpp"

#include <string>

namespace tl::secrets {

#ifdef _WIN32
// ---------------------------------------------------------------------------
// base64 (formats interoperables avec Convert.ToBase64String du C#)
// ---------------------------------------------------------------------------

std::string b64_encode(const unsigned char* data, std::size_t n) {
    DWORD chars = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(n), flags, nullptr, &chars))
        return {};
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(n), flags, out.data(), &chars))
        return {};
    out.resize(chars);
    return out;
}

std::optional<std::string> b64_decode(const std::string& s) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                              CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
        return std::nullopt;
    std::string out(n, '\0');
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                              CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(out.data()), &n, nullptr,
                              nullptr))
        return std::nullopt;
    out.resize(n);
    return out;
}

// DataProtectionScope.CurrentUser == CryptProtectData sans entropie.
std::optional<std::string> dpapi_protect_b64(const std::string& clear) {
    DATA_BLOB in{static_cast<DWORD>(clear.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(clear.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::string b64 = b64_encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (b64.empty()) return std::nullopt;
    return b64;
}

std::optional<std::string> dpapi_unprotect_b64(const std::string& b64) {
    auto raw = b64_decode(b64);
    if (!raw || raw->empty()) return std::nullopt;
    DATA_BLOB in{static_cast<DWORD>(raw->size()),
                 reinterpret_cast<BYTE*>(raw->data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::string clear(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return clear;
}

// Les std::string ne s'effacent pas a la destruction : tout secret dont la
// duree de vie est terminee est ecrase explicitement. SecureZeroMemory est
// opaque pour l'optimiseur (pas d'elision).
void secure_wipe(std::string& s) {
    if (!s.empty()) ::SecureZeroMemory(s.data(), s.size());
}

// Sous Windows le stockage est toujours DPAPI : jamais de repli.
bool insecure_fallback_active() { return false; }

#else // POSIX : base64 manuel + libsecret, repli fichier 0600

namespace fs = std::filesystem;

// base64 standard (même alphabet que Convert.ToBase64String, sans retours).
std::string b64_encode(const unsigned char* data, std::size_t n) {
    static const char kB64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((n + 2) / 3) * 4);
    for (std::size_t i = 0; i < n; i += 3) {
        const unsigned b0 = data[i];
        const unsigned b1 = i + 1 < n ? data[i + 1] : 0;
        const unsigned b2 = i + 2 < n ? data[i + 2] : 0;
        out.push_back(kB64[(b0 >> 2) & 0x3F]);
        out.push_back(kB64[((b0 << 4) | (b1 >> 4)) & 0x3F]);
        out.push_back(i + 1 < n ? kB64[((b1 << 2) | (b2 >> 6)) & 0x3F] : '=');
        out.push_back(i + 2 < n ? kB64[b2 & 0x3F] : '=');
    }
    return out;
}

std::optional<std::string> b64_decode(const std::string& s) {
    static const signed char kRev[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1, 0,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1};
    if (s.size() % 4 != 0) return std::nullopt;
    std::string out;
    out.reserve((s.size() / 4) * 3);
    for (std::size_t i = 0; i < s.size(); i += 4) {
        int v[4];
        int pad = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i + k];
            if (c == '=') {
                v[k] = 0;
                ++pad;
            } else {
                const int d = kRev[static_cast<unsigned char>(c)];
                if (d < 0) return std::nullopt;
                v[k] = d;
            }
        }
        if (pad > 2) return std::nullopt;
        const unsigned triple =
            (static_cast<unsigned>(v[0]) << 18) | (static_cast<unsigned>(v[1]) << 12) |
            (static_cast<unsigned>(v[2]) << 6) | static_cast<unsigned>(v[3]);
        out.push_back(static_cast<char>((triple >> 16) & 0xFF));
        if (pad < 2) out.push_back(static_cast<char>((triple >> 8) & 0xFF));
        if (pad < 1) out.push_back(static_cast<char>(triple & 0xFF));
    }
    return out;
}

// Le blob échangé (base64 de "ks:<uuid>" ou "local:<uuid>") ne contient
// jamais le secret : sous Windows le blob porte le chiffré DPAPI, sous Linux
// il ne porte qu'une référence (trousseau ou fichier local).
constexpr const char* kKsTag = "ks:";
constexpr const char* kLocalTag = "local:";

std::atomic<bool> g_insecure{false};

std::string gen_uuid() {
    unsigned char b[16] = {};
    std::size_t done = 0;
    while (done < sizeof(b)) {
        const ssize_t n = ::getrandom(b + done, sizeof(b) - done, 0);
        if (n <= 0) break;
        done += static_cast<std::size_t>(n);
    }
    if (done < sizeof(b)) {
        std::random_device rd;
        for (; done < sizeof(b); ++done) b[done] = static_cast<unsigned char>(rd());
    }
    char out[33];
    for (int i = 0; i < 16; ++i) std::snprintf(out + i * 2, 3, "%02x", b[i]);
    out[32] = '\0';
    return out;
}

fs::path local_store_path() {
    // Même racine que DataStore (XDG), sans en dépendre (stratification).
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg)
        return fs::path(xdg) / "TeamLauncher" / "secrets.local";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".local" / "share" / "TeamLauncher" / "secrets.local";
    return {};
}

// Schéma libsecret : un attribut "key" = uuid généré.
// Init champ par champ (les champs reserved varient selon la version ;
/// l'init agregat déclenche -Wmissing-field-initializers sous GCC).
const SecretSchema* tl_schema() {
    static SecretSchema s{};
    static std::once_flag once;
    std::call_once(once, [] {
        s.name = const_cast<char*>("org.teamlauncher.Secret");
        s.flags = SECRET_SCHEMA_NONE;
        s.attributes[0].name = const_cast<char*>("key");
        s.attributes[0].type = SECRET_SCHEMA_ATTRIBUTE_STRING;
    });
    return &s;
}

bool keyring_store(const std::string& uuid, const std::string& clear) {
    GError* err = nullptr;
    const gboolean ok = secret_password_store_sync(
        tl_schema(), SECRET_COLLECTION_DEFAULT, "Team Launcher", clear.c_str(),
        nullptr, &err, "key", uuid.c_str(), nullptr);
    if (err) g_error_free(err);
    return ok == TRUE;
}

std::optional<std::string> keyring_lookup(const std::string& uuid) {
    GError* err = nullptr;
    gchar* v = secret_password_lookup_sync(tl_schema(), nullptr, &err, "key",
                                           uuid.c_str(), nullptr);
    if (err) g_error_free(err);
    if (!v) return std::nullopt;
    std::string out(v);
    secret_password_free(v);
    return out;
}

// Repli : {"version":1,"secrets":{uuid:clair}} en 0600. Le flag insecure
// permet à l'UI d'avertir (jamais de clair silencieux).
std::optional<std::string> local_lookup(const std::string& uuid) {
    const fs::path p = local_store_path();
    if (p.empty()) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    try {
        const nlohmann::json j = nlohmann::json::parse(
            std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
        const auto it = j.find("secrets");
        if (it == j.end() || !it->is_object()) return std::nullopt;
        const auto kv = it->find(uuid);
        if (kv == it->end() || !kv->is_string()) return std::nullopt;
        g_insecure = true;
        return kv->get<std::string>();
    } catch (...) {
        return std::nullopt;
    }
}

bool local_store(const std::string& uuid, const std::string& clear) {
    const fs::path p = local_store_path();
    if (p.empty()) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    nlohmann::json j;
    {
        std::ifstream in(p, std::ios::binary);
        if (in) {
            try {
                j = nlohmann::json::parse(std::string(
                    (std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()));
            } catch (...) {
                j = nlohmann::json::object();
            }
        }
    }
    if (!j.is_object()) j = nlohmann::json::object();
    j["version"] = 1;
    j["secrets"][uuid] = clear;
    const std::string text = j.dump();
    {
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        out.flush();
        if (!out) return false;
    }
    ::chmod(p.string().c_str(), S_IRUSR | S_IWUSR);
    g_insecure = true;
    return true;
}

void secure_wipe(std::string& s) {
    if (s.empty()) return;
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); ++i) p[i] = 0;
}

// Même contrat que DPAPI côté appelants : trousseau d'abord, repli local.
// Le clair n'est jamais renvoyé tel quel en échec (encrypt_value garde
// l'ancienne valeur, comme sous Windows).
std::optional<std::string> dpapi_protect_b64(const std::string& clear) {
    const std::string uuid = gen_uuid();
    const std::string ks = std::string(kKsTag) + uuid;
    if (keyring_store(uuid, clear))
        return b64_encode(reinterpret_cast<const unsigned char*>(ks.c_str()), ks.size());
    const std::string lo = std::string(kLocalTag) + uuid;
    if (local_store(uuid, clear))
        return b64_encode(reinterpret_cast<const unsigned char*>(lo.c_str()), lo.size());
    return std::nullopt;
}

std::optional<std::string> dpapi_unprotect_b64(const std::string& b64) {
    auto raw = b64_decode(b64);
    if (!raw || raw->empty()) return std::nullopt;
    if (raw->rfind(kKsTag, 0) == 0) return keyring_lookup(raw->substr(3));
    if (raw->rfind(kLocalTag, 0) == 0) return local_lookup(raw->substr(6));
    return std::nullopt; // format inconnu (ex. blob DPAPI Windows)
}

bool insecure_fallback_active() { return g_insecure.load(); }

#endif // _WIN32 / POSIX

// ---------------------------------------------------------------------------
// Champs de config.json
// ---------------------------------------------------------------------------

bool is_encrypted(const std::string& v) {
    return v.compare(0, std::char_traits<char>::length(kEncPrefix), kEncPrefix) == 0;
}

std::string encrypt_value(const std::string& clear) {
    if (clear.empty()) return clear;
    auto b64 = dpapi_protect_b64(clear);
    if (!b64) return clear; // DPAPI indisponible : on n'ecrase jamais la valeur
    return std::string(kEncPrefix) + *b64;
}

std::string decrypt_value(const std::string& stored) {
    if (stored.empty() || !is_encrypted(stored)) return stored; // ancien format clair
    auto clear = dpapi_unprotect_b64(stored.substr(std::char_traits<char>::length(kEncPrefix)));
    if (!clear) return stored; // blob illisible : on garde le stockage brut
    return *clear;
}

} // namespace tl::secrets

#include "signing.hpp"

#include "datastore.hpp"
#include "secrets.hpp" // b64_decode

#include "monocypher-ed25519.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>

using nlohmann::json;
namespace fs = std::filesystem;

namespace tl::signing {

namespace {

// Tolérance d'horloge : un PC en avance ou en retard d'une journée ne doit
// pas rendre tous les plugins invalides.
constexpr std::int64_t kClockSkew = 24 * 3600;

// Bornes d'un dossier de plugin. Au-delà, on ne hache pas : un dossier
// démesuré est soit une erreur, soit une tentative de geler le launcher.
constexpr std::size_t kMaxFiles = 4096;
constexpr std::uintmax_t kMaxTotalBytes = 512ull * 1024 * 1024;
constexpr std::uintmax_t kMaxSigFile = 1024 * 1024;

const char* const kKnownUsages[] = {"plugin", "mod", "revocation"};

// --- Racines épinglées ------------------------------------------------------
//
// CLÉ(S) RACINE(S) PUBLIQUE(S), 32 octets chacune. À remplir avec la sortie
// de l'outil de cérémonie (tools/keys) : SEULE la clé PUBLIQUE va ici. La
// clé privée correspondante ne doit exister que sur un support hors ligne.
//
// Tant que cette liste est vide, aucune signature ne peut être vérifiée et
// tous les plugins sont « non signés ».
const std::vector<PublicKey> kBuiltinRoots = {};

std::string hex(const std::uint8_t* d, std::size_t n) {
    static const char* h = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        out.push_back(h[d[i] >> 4]);
        out.push_back(h[d[i] & 0xF]);
    }
    return out;
}

bool safe_key_id(const std::string& s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char c : s)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_' || c == '.'))
            return false;
    return true;
}

std::optional<std::string> b64(const json& j, const char* key) {
    const auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::nullopt;
    return secrets::b64_decode(it->get<std::string>());
}

// Vérifie { payload, sig } avec `pub` et rend le payload décodé.
std::optional<std::string> open_signed(const json& obj, const PublicKey& pub) {
    if (!obj.is_object()) return std::nullopt;
    const auto payload = b64(obj, "payload");
    const auto sig = b64(obj, "sig");
    if (!payload || !sig || sig->size() != 64) return std::nullopt;
    if (crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(sig->data()), pub.data(),
                             reinterpret_cast<const std::uint8_t*>(payload->data()),
                             payload->size()) != 0)
        return std::nullopt;
    return payload;
}

json parse_obj(const std::string& s) {
    json j = json::parse(s, nullptr, false);
    return j.is_object() ? j : json();
}

struct Cert {
    std::string keyId;
    PublicKey pub{};
    std::set<std::string> usages;
    std::int64_t notBefore = 0, notAfter = 0;
};

std::optional<Cert> open_cert(const json& signedCert, const Roots& roots, std::string* err) {
    std::optional<std::string> payload;
    for (const auto& r : roots.keys)
        if ((payload = open_signed(signedCert, r))) break;
    if (!payload) {
        *err = roots.keys.empty() ? "aucune clé racine configurée dans ce launcher"
                                  : "certificat non signé par une clé racine reconnue";
        return std::nullopt;
    }
    const json c = parse_obj(*payload);
    if (c.is_null() || c.value("type", "") != "tl-key-cert" || c.value("v", 0) != 1) {
        *err = "certificat mal formé";
        return std::nullopt;
    }
    Cert out;
    out.keyId = c.value("keyId", std::string{});
    const auto pk = b64(c, "publicKey");
    if (!safe_key_id(out.keyId) || !pk || pk->size() != 32) {
        *err = "certificat : identifiant ou clé invalide";
        return std::nullopt;
    }
    std::copy(pk->begin(), pk->end(), out.pub.begin());
    if (!c.contains("notBefore") || !c["notBefore"].is_number_integer() ||
        !c.contains("notAfter") || !c["notAfter"].is_number_integer()) {
        *err = "certificat : période de validité absente";
        return std::nullopt;
    }
    out.notBefore = c["notBefore"].get<std::int64_t>();
    out.notAfter = c["notAfter"].get<std::int64_t>();
    if (out.notAfter <= out.notBefore) {
        *err = "certificat : période de validité incohérente";
        return std::nullopt;
    }
    if (const auto u = c.find("usages"); u != c.end() && u->is_array())
        for (const auto& v : *u)
            if (v.is_string())
                for (const char* k : kKnownUsages)
                    if (v.get<std::string>() == k) out.usages.insert(k);
    return out;
}

// Un horodatage déclaré par le signataire est-il plausible pour ce
// certificat ? Il doit tomber dans sa période de validité, et pas dans le
// futur. Un intermédiaire compromis pourrait antidater : c'est la liste
// de révocation (keyIds) qui ferme ce cas.
bool within(const Cert& c, std::int64_t t, std::int64_t now) {
    return t >= c.notBefore - kClockSkew && t <= c.notAfter + kClockSkew &&
           t <= now + kClockSkew;
}

} // namespace

const Roots& builtin_roots() {
    static const Roots r{kBuiltinRoots};
    return r;
}

std::string sha512_hex(const std::string& data) {
    std::uint8_t h[64];
    crypto_sha512(h, reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    return hex(h, 64);
}

std::optional<std::string> sha512_file_hex(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    crypto_sha512_ctx ctx;
    crypto_sha512_init(&ctx);
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const auto n = in.gcount();
        if (n > 0)
            crypto_sha512_update(&ctx, reinterpret_cast<const std::uint8_t*>(buf.data()),
                                 static_cast<std::size_t>(n));
    }
    if (in.bad()) return std::nullopt;
    std::uint8_t h[64];
    crypto_sha512_final(&ctx, h);
    return hex(h, 64);
}

// --- Révocations ------------------------------------------------------------

std::optional<Revocations> parse_revocations(const std::string& text, const Roots& roots,
                                             std::int64_t now, std::int64_t minSeq,
                                             std::string* err) {
    std::string e;
    auto fail = [&](const std::string& m) -> std::optional<Revocations> {
        if (err) *err = m;
        return std::nullopt;
    };
    const json doc = parse_obj(text);
    if (doc.is_null() || doc.value("v", 0) != 1 || !doc.contains("cert") ||
        !doc.contains("list"))
        return fail("liste de révocation mal formée");
    const auto cert = open_cert(doc["cert"], roots, &e);
    if (!cert) return fail(e);
    if (!cert->usages.count("revocation"))
        return fail("certificat non habilité à signer une liste de révocation");
    const auto payload = open_signed(doc["list"], cert->pub);
    if (!payload) return fail("signature de la liste de révocation invalide");
    const json l = parse_obj(*payload);
    if (l.is_null() || l.value("type", "") != "tl-revocations" || l.value("v", 0) != 1 ||
        !l.contains("seq") || !l["seq"].is_number_integer() || !l.contains("issuedAt") ||
        !l["issuedAt"].is_number_integer())
        return fail("contenu de la liste de révocation mal formé");

    Revocations r;
    r.seq = l["seq"].get<std::int64_t>();
    r.issuedAt = l["issuedAt"].get<std::int64_t>();
    if (r.seq < 0) return fail("numéro de liste négatif");
    if (r.seq < minSeq)
        return fail("liste de révocation plus ancienne que la dernière acceptée (rejeu ?)");
    if (!within(*cert, r.issuedAt, now)) return fail("liste émise hors de la validité du certificat");
    r.killAll = l.value("killAll", false);
    if (const auto k = l.find("keyIds"); k != l.end() && k->is_array())
        for (const auto& v : *k)
            if (v.is_string()) r.keyIds.insert(v.get<std::string>());
    if (const auto rel = l.find("releases"); rel != l.end() && rel->is_array())
        for (const auto& v : *rel)
            if (v.is_object())
                r.releases.emplace(v.value("id", std::string{}), v.value("version", std::string{}));
    if (const auto h = l.find("fileHashes"); h != l.end() && h->is_array())
        for (const auto& v : *h)
            if (v.is_string()) r.fileHashes.insert(v.get<std::string>());
    // Le certificat qui a signé cette liste est-il lui-même révoqué par
    // elle ? Une clé compromise ne doit pas pouvoir publier la liste.
    if (r.keyIds.count(cert->keyId))
        return fail("liste signée par un certificat qu'elle révoque elle-même");
    return r;
}

namespace {

std::mutex g_revM;
Revocations g_rev;
bool g_revLoaded = false;

fs::path rev_cache() { return DataStore::dir() / "revocations.json"; }

void load_cache_locked() {
    if (g_revLoaded) return;
    g_revLoaded = true;
    std::ifstream in(rev_cache(), std::ios::binary);
    if (!in) return;
    std::stringstream ss;
    ss << in.rdbuf();
    std::string err;
    // Au chargement, on n'impose pas d'horodatage minimal ; la vérification
    // de signature suffit à écarter un fichier forgé.
    if (auto r = parse_revocations(ss.str(), builtin_roots(),
                                   static_cast<std::int64_t>(std::time(nullptr)), -1, &err))
        g_rev = *r;
}

} // namespace

Revocations current_revocations() {
    std::lock_guard<std::mutex> lk(g_revM);
    load_cache_locked();
    return g_rev;
}

bool install_revocations(const std::string& text, std::int64_t now, std::string* err) {
    std::lock_guard<std::mutex> lk(g_revM);
    load_cache_locked();
    auto r = parse_revocations(text, builtin_roots(), now, g_rev.seq, err);
    if (!r) return false;
    if (r->seq == g_rev.seq) return true; // déjà en vigueur
    g_rev = *r;
    std::ofstream out(rev_cache(), std::ios::binary | std::ios::trunc);
    out << text;
    return true;
}

// --- Plugin -------------------------------------------------------------------

Verdict verify_plugin_dir(const fs::path& dir, const std::string& pluginId,
                          const std::string& version, const Roots& roots,
                          const Revocations* revs, std::int64_t now) {
    Verdict v;
    auto invalid = [&](const std::string& why) {
        v.status = Status::Invalid;
        v.detail = why;
        return v;
    };

    if (revs && revs->killAll) {
        v.status = Status::Revoked;
        v.detail = "arrêt d'urgence décidé à distance";
        return v;
    }

    std::error_code ec;
    const fs::path sigPath = dir / "signature.json";
    if (!fs::exists(sigPath, ec)) {
        v.status = Status::Unsigned;
        v.detail = "pas de signature";
        return v;
    }
    if (fs::is_symlink(sigPath, ec) || !fs::is_regular_file(sigPath, ec))
        return invalid("signature.json n'est pas un fichier ordinaire");
    if (fs::file_size(sigPath, ec) > kMaxSigFile) return invalid("signature.json démesuré");

    std::string sigText;
    {
        std::ifstream in(sigPath, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        sigText = ss.str();
    }
    const json doc = parse_obj(sigText);
    if (doc.is_null() || doc.value("v", 0) != 1 || !doc.contains("cert") ||
        !doc.contains("release"))
        return invalid("signature.json mal formé");

    std::string err;
    const auto cert = open_cert(doc["cert"], roots, &err);
    if (!cert) {
        // Sans racine configurée, on ne PEUT pas vérifier : ce n'est pas une
        // signature fausse, c'est une signature invérifiable. On le traite
        // comme « non signé » plutôt que d'accuser le plugin.
        if (roots.keys.empty()) {
            v.status = Status::Unsigned;
            v.detail = err;
            return v;
        }
        return invalid(err);
    }
    if (!cert->usages.count("plugin")) return invalid("certificat non habilité à signer des plugins");
    v.keyId = cert->keyId;
    if (revs && revs->keyIds.count(cert->keyId)) {
        v.status = Status::Revoked;
        v.detail = "clé de signature révoquée (" + cert->keyId + ")";
        return v;
    }

    const auto payload = open_signed(doc["release"], cert->pub);
    if (!payload) return invalid("signature de publication invalide");
    const json rel = parse_obj(*payload);
    if (rel.is_null() || rel.value("type", "") != "tl-plugin-release" || rel.value("v", 0) != 1)
        return invalid("publication mal formée");
    if (rel.value("pluginId", "") != pluginId)
        return invalid("signature émise pour un autre plugin (« " + rel.value("pluginId", "") + " »)");
    if (rel.value("version", "") != version)
        return invalid("signature émise pour une autre version (« " + rel.value("version", "") + " »)");
    if (rel.value("keyId", "") != cert->keyId) return invalid("publication et certificat discordants");
    if (!rel.contains("signedAt") || !rel["signedAt"].is_number_integer() ||
        !within(*cert, rel["signedAt"].get<std::int64_t>(), now))
        return invalid("signature hors de la période de validité du certificat");
    if (revs && revs->releases.count({pluginId, version})) {
        v.status = Status::Revoked;
        v.detail = "cette version a été révoquée";
        return v;
    }

    const auto files = rel.find("files");
    if (files == rel.end() || !files->is_object() || files->empty())
        return invalid("publication sans liste de fichiers");

    // Fichiers RÉELLEMENT présents : chaque fichier doit être signé, et
    // chaque fichier signé doit être là, intact.
    std::map<std::string, fs::path> present;
    std::uintmax_t total = 0;
    for (auto it = fs::recursive_directory_iterator(dir, fs::directory_options::none, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) return invalid("dossier illisible");
        // Type EXACT de l'entrée, sans suivre les liens. Sous Windows, une
        // jonction n'est pas un « symlink » au sens de is_symlink(), et
        // l'itérateur récursif la suivrait hors du dossier. Tout ce qui
        // n'est ni un fichier ordinaire ni un dossier ordinaire est refusé.
        const auto t = it->symlink_status(ec).type();
        if (ec) return invalid("dossier illisible");
        if (t == fs::file_type::directory) continue;
        if (t != fs::file_type::regular)
            return invalid("lien ou entrée spéciale dans le dossier du plugin");
        // UTF-8, PAS generic_string() : sous Windows, celle-ci convertit dans la
        // page de code ANSI — un nom accentué ne correspondrait plus à celui
        // signé par le serveur, et un caractère hors page de code lèverait.
        const auto u8 = fs::relative(it->path(), dir, ec).generic_u8string();
        const std::string rel_(u8.begin(), u8.end());
        if (ec || rel_.empty()) return invalid("chemin illisible");
        if (rel_ == "signature.json") continue;
        if (present.size() >= kMaxFiles) return invalid("trop de fichiers");
        total += fs::file_size(it->path(), ec);
        if (total > kMaxTotalBytes) return invalid("dossier trop volumineux");
        present[rel_] = it->path();
    }
    for (const auto& [name, _] : present)
        if (!files->contains(name)) return invalid("fichier non signé : " + name);
    for (auto it = files->begin(); it != files->end(); ++it) {
        if (!it.value().is_string()) return invalid("empreinte mal formée");
        const auto p = present.find(it.key());
        if (p == present.end()) return invalid("fichier signé absent : " + it.key());
        const auto h = sha512_file_hex(p->second);
        if (!h) return invalid("fichier illisible : " + it.key());
        if (*h != it.value().get<std::string>()) return invalid("fichier modifié : " + it.key());
        if (revs && revs->fileHashes.count(*h)) {
            v.status = Status::Revoked;
            v.detail = "fichier révoqué : " + it.key();
            return v;
        }
    }

    v.status = Status::Valid;
    v.detail = "signé par " + cert->keyId;
    return v;
}

} // namespace tl::signing

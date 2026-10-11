// Signature des plugins (V8).
//
// Les clés sont fabriquées ici, à partir de graines fixes : le test
// construit de vrais certificats, de vraies publications et de vraies
// listes de révocation, les signe, puis les fait vérifier par le code du
// launcher. On vérifie surtout les REFUS : une vérification de signature
// qui accepte trop est pire qu'aucune.

#include "secrets.hpp"
#include "signing.hpp"

#include "monocypher-ed25519.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

using nlohmann::json;
namespace fs = std::filesystem;
using namespace tl;
using signing::Status;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

struct Key {
    std::uint8_t sk[64];
    signing::PublicKey pk;
};

Key make_key(std::uint8_t seedByte) {
    std::uint8_t seed[32];
    std::memset(seed, seedByte, sizeof(seed));
    Key k;
    crypto_ed25519_key_pair(k.sk, k.pk.data(), seed); // efface la graine
    return k;
}

std::string b64(const std::string& s) {
    return secrets::b64_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

json sign_obj(const std::string& payload, const Key& k) {
    std::uint8_t sig[64];
    crypto_ed25519_sign(sig, k.sk, reinterpret_cast<const std::uint8_t*>(payload.data()),
                        payload.size());
    return {{"payload", b64(payload)},
            {"sig", secrets::b64_encode(sig, sizeof(sig))}};
}

json make_cert(const Key& root, const Key& inter, const std::string& keyId,
               const std::vector<std::string>& usages, std::int64_t nb = 1000,
               std::int64_t na = 4000000000) {
    const std::string pub(reinterpret_cast<const char*>(inter.pk.data()), 32);
    const json c{{"type", "tl-key-cert"}, {"v", 1},          {"keyId", keyId},
                 {"publicKey", b64(pub)}, {"usages", usages}, {"notBefore", nb},
                 {"notAfter", na}};
    return sign_obj(c.dump(), root);
}

void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// Écrit signature.json pour le contenu ACTUEL du dossier.
void sign_dir(const fs::path& dir, const std::string& id, const std::string& ver,
              const Key& root, const Key& inter, const std::string& keyId = "int-1",
              std::int64_t signedAt = 2000000000,
              const std::vector<std::string>& usages = {"plugin"}) {
    json files = json::object();
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = fs::relative(e.path(), dir).generic_string();
        if (rel == "signature.json") continue;
        files[rel] = *signing::sha512_file_hex(e.path());
    }
    const json release{{"type", "tl-plugin-release"}, {"v", 1}, {"pluginId", id},
                       {"version", ver}, {"keyId", keyId}, {"signedAt", signedAt},
                       {"files", files}};
    const json doc{{"v", 1},
                   {"cert", make_cert(root, inter, keyId, usages)},
                   {"release", sign_obj(release.dump(), inter)}};
    write(dir / "signature.json", doc.dump());
}

std::string make_revocations(const Key& root, const Key& inter, std::int64_t seq,
                             const json& extra = json::object(),
                             const std::vector<std::string>& usages = {"revocation"},
                             const std::string& keyId = "int-rev") {
    json list{{"type", "tl-revocations"}, {"v", 1}, {"seq", seq}, {"issuedAt", 2000000000}};
    for (auto it = extra.begin(); it != extra.end(); ++it) list[it.key()] = it.value();
    const json doc{{"v", 1},
                   {"cert", make_cert(root, inter, keyId, usages)},
                   {"list", sign_obj(list.dump(), inter)}};
    return doc.dump();
}

const std::int64_t kNow = 2000000100;

} // namespace

int main() {
    // =====================================================================
    // 0. Vecteurs de référence : l'algorithme est bien le standard
    // =====================================================================
    CHECK(signing::sha512_hex("abc") ==
          "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a"
          "2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f");
    {
        // RFC 8032, section 7.1, TEST 1 (message vide). Garantit qu'une
        // signature produite côté serveur par PyNaCl sera vérifiée ici.
        auto unhex = [](const char* h, std::uint8_t* out, std::size_t n) {
            for (std::size_t i = 0; i < n; ++i) {
                unsigned v = 0;
                std::sscanf(h + 2 * i, "%2x", &v);
                out[i] = static_cast<std::uint8_t>(v);
            }
        };
        std::uint8_t seed[32], pk[32], sig[64], sk[64], derived[32];
        unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", seed, 32);
        unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", pk, 32);
        unhex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
              "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
              sig, 64);
        crypto_ed25519_key_pair(sk, derived, seed);
        CHECK(std::memcmp(derived, pk, 32) == 0);
        CHECK(crypto_ed25519_check(sig, pk, nullptr, 0) == 0);
        sig[0] ^= 1;
        CHECK(crypto_ed25519_check(sig, pk, nullptr, 0) != 0);
    }

    const Key root = make_key(1), inter = make_key(2), other = make_key(3);
    const signing::Roots roots{{root.pk}};
    const fs::path base = fs::temp_directory_path() / "tl_test_signing";
    std::error_code ec;
    fs::remove_all(base, ec);

    auto fresh = [&](const std::string& name) {
        const fs::path d = base / name;
        write(d / "plugin.json", R"({"name":"X","version":"1.0"})");
        write(d / "bin" / "service.exe", "binaire factice");
        return d;
    };
    auto verdict = [&](const fs::path& d, const signing::Revocations* r = nullptr,
                       const std::string& id = "demo", const std::string& ver = "1.0") {
        return signing::verify_plugin_dir(d, id, ver, roots, r, kNow);
    };

    // =====================================================================
    // 1. Cas nominaux
    // =====================================================================
    {
        const auto d = fresh("nosig");
        CHECK(verdict(d).status == Status::Unsigned);
    }
    {
        const auto d = fresh("ok");
        sign_dir(d, "demo", "1.0", root, inter);
        const auto v = verdict(d);
        CHECK(v.status == Status::Valid);
        CHECK(v.keyId == "int-1");
        // Sans racine configurée : invérifiable, donc « non signé » — pas
        // une accusation de falsification.
        CHECK(signing::verify_plugin_dir(d, "demo", "1.0", signing::Roots{}, nullptr, kNow)
                  .status == Status::Unsigned);
    }

    // =====================================================================
    // 2. Refus
    // =====================================================================
    {
        // Certificat signé par une clé qui n'est pas une racine.
        const auto d = fresh("fauxroot");
        sign_dir(d, "demo", "1.0", other, inter);
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // Fichier modifié APRÈS signature.
        const auto d = fresh("modifie");
        sign_dir(d, "demo", "1.0", root, inter);
        write(d / "bin" / "service.exe", "binaire remplacé");
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // DLL déposée à côté de l'exécutable signé : LE cas que la règle
        // « liste exacte » ferme.
        const auto d = fresh("dll");
        sign_dir(d, "demo", "1.0", root, inter);
        write(d / "bin" / "version.dll", "charge utile");
        const auto v = verdict(d);
        CHECK(v.status == Status::Invalid);
        CHECK(v.detail.find("non signé") != std::string::npos);
    }
    {
        // Fichier signé disparu.
        const auto d = fresh("absent");
        sign_dir(d, "demo", "1.0", root, inter);
        fs::remove(d / "bin" / "service.exe");
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // Signature d'un AUTRE plugin, ou d'une autre version, recopiée.
        const auto d = fresh("autreid");
        sign_dir(d, "autre", "1.0", root, inter);
        CHECK(verdict(d).status == Status::Invalid);
        sign_dir(d, "demo", "0.9", root, inter);
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // Certificat sans l'usage « plugin ».
        const auto d = fresh("usage");
        sign_dir(d, "demo", "1.0", root, inter, "int-1", 2000000000, {"mod"});
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // Signé hors de la période de validité du certificat (cert valide à
        // partir de 1000). L'écart doit dépasser la tolérance d'horloge
        // d'une journée, voulue : 500 tombait dedans et passait — à raison.
        const auto d = fresh("periode");
        sign_dir(d, "demo", "1.0", root, inter, "int-1", 1000 - 3 * 24 * 3600);
        CHECK(verdict(d).status == Status::Invalid);
        // Ni dans le futur.
        sign_dir(d, "demo", "1.0", root, inter, "int-1", kNow + 10 * 24 * 3600);
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // Charge utile remplacée en gardant l'ancienne signature.
        const auto d = fresh("swap");
        sign_dir(d, "demo", "1.0", root, inter);
        std::ifstream in(d / "signature.json");
        json doc = json::parse(in);
        in.close();
        doc["release"]["payload"] = b64(R"({"type":"tl-plugin-release","v":1})");
        write(d / "signature.json", doc.dump());
        CHECK(verdict(d).status == Status::Invalid);
    }
    {
        // signature.json illisible.
        const auto d = fresh("illisible");
        write(d / "signature.json", "{pas du json");
        CHECK(verdict(d).status == Status::Invalid);
    }
#ifndef _WIN32
    {
        // Lien symbolique vers l'extérieur du dossier.
        const auto d = fresh("lien");
        sign_dir(d, "demo", "1.0", root, inter);
        fs::create_symlink("/etc/hostname", d / "bin" / "lien", ec);
        if (!ec) CHECK(verdict(d).status == Status::Invalid);
    }
#endif

    // =====================================================================
    // 3. Révocation
    // =====================================================================
    {
        const auto d = fresh("rev");
        sign_dir(d, "demo", "1.0", root, inter);
        signing::Revocations r;
        r.keyIds = {"int-1"};
        CHECK(verdict(d, &r).status == Status::Revoked);
        r = {};
        r.releases = {{"demo", "1.0"}};
        CHECK(verdict(d, &r).status == Status::Revoked);
        r = {};
        r.fileHashes = {*signing::sha512_file_hex(d / "bin" / "service.exe")};
        CHECK(verdict(d, &r).status == Status::Revoked);
        r = {};
        r.killAll = true;
        CHECK(verdict(d, &r).status == Status::Revoked);
        // Révoquer une autre version ne touche pas celle-ci.
        r = {};
        r.releases = {{"demo", "0.9"}};
        CHECK(verdict(d, &r).status == Status::Valid);
    }
    {
        std::string err;
        const auto ok = signing::parse_revocations(
            make_revocations(root, inter, 5, {{"keyIds", {"int-1"}}, {"killAll", false}}),
            roots, kNow, -1, &err);
        CHECK(ok.has_value());
        if (ok) {
            CHECK(ok->seq == 5);
            CHECK(ok->keyIds.count("int-1") == 1);
        }
        // Rejeu d'une liste plus ancienne que la dernière acceptée.
        CHECK(!signing::parse_revocations(make_revocations(root, inter, 4), roots, kNow, 5, &err));
        CHECK(err.find("ancienne") != std::string::npos);
        // Certificat non habilité à révoquer.
        CHECK(!signing::parse_revocations(make_revocations(root, inter, 6, {}, {"plugin"}), roots,
                                          kNow, -1, &err));
        // Liste signée par une clé qu'elle révoque elle-même.
        CHECK(!signing::parse_revocations(
            make_revocations(root, inter, 7, {{"keyIds", {"int-rev"}}}), roots, kNow, -1, &err));
        // Certificat de la liste signé hors racine.
        CHECK(!signing::parse_revocations(make_revocations(other, inter, 8), roots, kNow, -1, &err));
        // Liste altérée après signature.
        std::string t = make_revocations(root, inter, 9);
        json doc = json::parse(t);
        doc["list"]["payload"] = b64(R"({"type":"tl-revocations","v":1,"seq":99,"issuedAt":2000000000,"killAll":true})");
        CHECK(!signing::parse_revocations(doc.dump(), roots, kNow, -1, &err));
    }

    fs::remove_all(base, ec);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

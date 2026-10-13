// Signature des plugins (V8).
//
// Les clés sont fabriquées ici, à partir de graines fixes : le test
// construit de vrais certificats, de vraies publications et de vraies
// listes de révocation, les signe, puis les fait vérifier par le code du
// launcher. On vérifie surtout les REFUS : une vérification de signature
// qui accepte trop est pire qu'aucune.

#include "datastore.hpp"
#include "secrets.hpp"
#include "signing.hpp"

#include "monocypher-ed25519.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
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
    // Daté dans le passé : la section M3 recharge via l'heure réelle, et
    // `within` refuse une liste émise « dans le futur ». Ne gêne pas les
    // autres tests, qui passent kNow.
    json list{{"type", "tl-revocations"}, {"v", 1}, {"seq", seq}, {"issuedAt", 1700000000}};
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

    // Racine compilée dans le launcher : exactement celle de la cérémonie.
    // La racine jetée le même jour (code papier exposé) ne doit jamais
    // revenir, même par erreur de copier-coller.
    {
        const signing::PublicKey expected{
            0xb9, 0x33, 0x59, 0xe2, 0xce, 0x1a, 0xc0, 0x4b, 0xf8, 0x39, 0xb0,
            0xae, 0xd2, 0xaf, 0xb7, 0xd4, 0x70, 0x50, 0x1d, 0x3c, 0x26, 0xac,
            0x27, 0xb3, 0x6c, 0x6c, 0xe0, 0x1e, 0x2f, 0xda, 0x81, 0xb3};
        const signing::PublicKey burned{
            0x58, 0x95, 0xb8, 0x0e, 0x63, 0x6f, 0xdd, 0xe2, 0x97, 0x65, 0x3e,
            0xa1, 0x46, 0x41, 0x48, 0x6d, 0x18, 0x35, 0x4a, 0x6a, 0x57, 0x05,
            0x1e, 0x29, 0xc0, 0x26, 0x67, 0x55, 0xd4, 0x46, 0x36, 0xf6};
        const auto& keys = signing::builtin_roots().keys;
        CHECK(keys.size() == 1 && keys[0] == expected);
        for (const auto& k : keys) CHECK(k != burned);
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

    // =====================================================================
    // 5. Interopérabilité : ce que signe le SERVEUR (Python, PyNaCl)
    // =====================================================================
    // tests/interop est produit par marketplace/tools/interop_fixture.py,
    // racine = make_key(1). Le test Python exige que le serveur le
    // reproduise à l'octet près ; ici, que le launcher l'accepte.
    {
        const fs::path src = fs::path(TL_INTEROP_DIR);
        const fs::path d = base / "interop";
        fs::copy(src / "plugin", d, fs::copy_options::recursive, ec);
        CHECK(!ec);
        const auto v = signing::verify_plugin_dir(d, "interop", "1.0.0", roots, nullptr, kNow);
        CHECK(v.status == Status::Valid);
        if (v.status != Status::Valid) std::printf("  interop : %s\n", v.detail.c_str());
        CHECK(v.keyId == "py-int-1");

        auto slurp = [](const fs::path& p) {
            std::ifstream in(p, std::ios::binary);
            return std::string(std::istreambuf_iterator<char>(in), {});
        };
        std::string err;
        const auto r7 = signing::parse_revocations(slurp(src / "revocations-7.json"), roots, kNow,
                                                   -1, &err);
        CHECK(r7 && r7->seq == 7 && !r7->killAll && r7->fileHashes.size() == 1);
        if (!r7) std::printf("  interop rev7 : %s\n", err.c_str());
        const auto r8 = signing::parse_revocations(slurp(src / "revocations-8.json"), roots, kNow,
                                                   7, &err);
        CHECK(r8 && r8->seq == 8);
        // Pas de retour en arrière vers la 7 une fois la 8 acceptée.
        CHECK(!signing::parse_revocations(slurp(src / "revocations-7.json"), roots, kNow, 8, &err));
        if (r7)
            CHECK(signing::verify_plugin_dir(d, "interop", "1.0.0", roots, &*r7, kNow).status ==
                  Status::Valid);
        if (r8)
            CHECK(signing::verify_plugin_dir(d, "interop", "1.0.0", roots, &*r8, kNow).status ==
                  Status::Revoked);
    }

    // =====================================================================
    // 6. Auto-update signé (verify_update) — H1
    // =====================================================================
    {
        const fs::path pkg = base / "update.zip";
        write(pkg, "contenu du paquet de mise a jour");
        const std::string sha = signing::sha512_hex("contenu du paquet de mise a jour");
        auto sig = [&](const std::vector<std::string>& usages, const std::string& shaOver) {
            const json rel{{"type", "tl-launcher-release"}, {"v", 1}, {"version", "9.9.9"},
                           {"keyId", "int-up"}, {"signedAt", 2000000000},
                           {"sha512", shaOver.empty() ? sha : shaOver}};
            return json{{"v", 1}, {"cert", make_cert(root, inter, "int-up", usages)},
                        {"release", sign_obj(rel.dump(), inter)}}.dump();
        };
        // Signée par une clé « launcher », empreinte correcte -> installable.
        CHECK(signing::verify_update(pkg, sig({"launcher"}, ""), roots, kNow).status ==
              Status::Valid);
        // Certificat sans usage « launcher » -> refus.
        CHECK(signing::verify_update(pkg, sig({"plugin", "mod"}, ""), roots, kNow).status ==
              Status::Invalid);
        // Empreinte qui ne correspond pas au paquet (paquet substitué) -> refus.
        CHECK(signing::verify_update(pkg, sig({"launcher"}, std::string(128, 'a')), roots, kNow)
                  .status == Status::Invalid);
        // Signé hors racine -> refus.
        {
            const json rel{{"type", "tl-launcher-release"}, {"v", 1}, {"version", "9.9.9"},
                           {"keyId", "int-up"}, {"signedAt", 2000000000}, {"sha512", sha}};
            const std::string bad =
                json{{"v", 1}, {"cert", make_cert(other, inter, "int-up", {"launcher"})},
                     {"release", sign_obj(rel.dump(), inter)}}.dump();
            CHECK(signing::verify_update(pkg, bad, roots, kNow).status == Status::Invalid);
        }
        // Sans racine compilée -> non vérifiable, donc refus (Unsigned, pas Valid).
        CHECK(signing::verify_update(pkg, sig({"launcher"}, ""), signing::Roots{}, kNow).status ==
              Status::Unsigned);
        // Document mal formé -> refus.
        CHECK(signing::verify_update(pkg, "pas du json", roots, kNow).status == Status::Invalid);

        // M6 : JSON au mauvais TYPE (le .sig vient du reseau, non verifie a ce
        // stade). Avant correctif, .value("v", 0) sur « v » non entier levait
        // type_error -> crash. Desormais : Invalid, jamais d'exception.
        CHECK(signing::verify_update(pkg, R"({"v":"pas un entier"})", roots, kNow).status ==
              Status::Invalid);
        CHECK(signing::verify_update(pkg, R"({"v":1,"cert":123,"release":"x"})", roots, kNow)
                  .status == Status::Invalid);
        CHECK(signing::verify_update(pkg, R"([1,2,3])", roots, kNow).status == Status::Invalid);
        CHECK(signing::verify_update(pkg, R"("juste une chaine")", roots, kNow).status ==
              Status::Invalid);
        // M6 : liste de révocation au mauvais type -> rejetee, pas de throw.
        std::string rerr;
        CHECK(!signing::parse_revocations(R"({"v":[1]})", roots, kNow, 0, &rerr).has_value());
        CHECK(!signing::parse_revocations(R"({"v":1,"cert":1,"list":2})", roots, kNow, 0, &rerr)
                   .has_value());
    }

    // =====================================================================
    // 7. M3 : révocations fail-closed (plancher persistant + sauvegarde config)
    // =====================================================================
    {
        const fs::path dataDir = base / "data";
        fs::create_directories(dataDir);
#ifdef _WIN32
        _putenv_s("TL_DATA_DIR", dataDir.string().c_str());
#else
        setenv("TL_DATA_DIR", dataDir.string().c_str(), 1);
#endif
        DataStore::settings.revocationFloorSeq = 0;
        DataStore::settings.revocationDoc.clear();
        signing::reset_revocations_for_tests();

        // L'équipe publie la liste n°2 qui révoque demo@1.0.
        const json rel = json::array({json{{"id", "demo"}, {"version", "1.0"}}});
        const std::string rev2 = make_revocations(root, inter, 2, {{"releases", rel}});
        std::string ierr;
        CHECK(signing::install_revocations(rev2, kNow, &ierr, roots));
        CHECK(signing::current_revocations(roots).releases.count({"demo", "1.0"}) == 1);

        // ATTAQUE 1 : suppression du fichier de cache -> la sauvegarde dans
        // la config garde la révocation (fail-CLOSED, avant : fail-open).
        fs::remove(dataDir / "revocations.json", ec);
        signing::reset_revocations_for_tests();
        CHECK(signing::current_revocations(roots).releases.count({"demo", "1.0"}) == 1);

        // ATTAQUE 2 : rejouer une ANCIENNE liste (n°1) -> refusée par le
        // plancher persistant (séquence courante = 2).
        const std::string rev1 = make_revocations(root, inter, 1, json::object());
        signing::reset_revocations_for_tests();
        CHECK(!signing::install_revocations(rev1, kNow, &ierr, roots));
        CHECK(signing::current_revocations(roots).seq == 2);
    }

    fs::remove_all(base, ec);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

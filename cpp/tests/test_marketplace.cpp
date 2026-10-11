// Marketplace côté launcher (V8) : adresse, catalogue, extraction stricte,
// et surtout installation — qui ne doit RIEN laisser passer que la chaîne
// de confiance n'a pas signé.
//
// Les paquets sont construits à partir de tests/interop, signé par le
// serveur Python (racine = graine 0x01…, comme make_key(1) ci-dessous).

#include "marketplace.hpp"

#include "miniz.h"
#include "monocypher-ed25519.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <string>

namespace fs = std::filesystem;
using namespace tl;
using signing::Kind;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

namespace {

const std::int64_t kNow = 2000000100;

signing::PublicKey root_key() {
    std::uint8_t seed[32], sk[64];
    std::memset(seed, 1, sizeof(seed));
    signing::PublicKey pk;
    crypto_ed25519_key_pair(sk, pk.data(), seed);
    return pk;
}

std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

using Files = std::map<std::string, std::string>; // nom UTF-8 -> contenu

Files read_dir(const fs::path& dir) {
    Files f;
    for (const auto& e : fs::recursive_directory_iterator(dir)) {
        if (!e.is_regular_file()) continue;
        const auto u = fs::relative(e.path(), dir).generic_u8string();
        f[std::string(u.begin(), u.end())] = slurp(e.path());
    }
    return f;
}

// Archive en mémoire, noms en UTF-8 tels quels.
std::string make_zip(const Files& files) {
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    for (const auto& [name, data] : files)
        mz_zip_writer_add_mem(&z, name.c_str(), data.data(), data.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t size = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &size);
    std::string out(static_cast<const char*>(buf), size);
    mz_free(buf);
    mz_zip_writer_end(&z);
    return out;
}

bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

} // namespace

int main() {
    // --- Adresse du serveur ----------------------------------------------------
    {
        std::string err;
        CHECK(market::normalize_base("https://market.example.org/", &err) ==
              std::optional<std::string>("https://market.example.org"));
        CHECK(market::normalize_base("http://127.0.0.1:8300", &err).has_value());
        CHECK(market::normalize_base("http://localhost:8300/", &err).has_value());
        CHECK(market::normalize_base("http://[::1]:8300", &err).has_value());
        CHECK(!market::normalize_base("http://market.example.org", &err));
        CHECK(contains(err, "HTTPS"));
        CHECK(!market::normalize_base("http://127.0.0.1.evil.org", &err));
        CHECK(!market::normalize_base("https://user:pw@market.example.org", &err));
        CHECK(!market::normalize_base("https://market.example.org/?x=1", &err));
        CHECK(!market::normalize_base("ftp://market.example.org", &err));
        CHECK(!market::normalize_base("https://", &err));
        CHECK(!market::normalize_base("https:///chemin", &err));
        CHECK(!market::normalize_base("", &err));
    }

    // --- Catalogue -------------------------------------------------------------
    {
        std::string err;
        const auto c = market::parse_catalog(R"([
            {"kind":"plugin","slug":"hello","name":"Hello","version":"1.0.0",
             "description":"d","permissions":["instances:read","game:launch"]},
            {"kind":"mod","slug":"coolmod","name":"Cool","version":"1.0"},
            {"kind":"plugin","slug":"../evil","name":"x","version":"1"},
            {"kind":"plugin","slug":"ok2","name":"x","version":"1 && rm"},
            {"kind":"theme","slug":"t","version":"1"},
            {"kind":"plugin","slug":"hello","name":"Doublon","version":"9"},
            42, null
        ])", &err);
        CHECK(err.empty());
        CHECK(c.size() == 2);
        if (c.size() == 2) {
            CHECK(c[0].slug == "hello" && c[0].kind == Kind::Plugin && c[0].permissions.size() == 2);
            CHECK(c[1].slug == "coolmod" && c[1].kind == Kind::Mod && c[1].name == "Cool");
        }
        CHECK(market::parse_catalog("{\"pas\":\"un tableau\"}", &err).empty() && !err.empty());
        err.clear();
        CHECK(market::parse_catalog("[{\"kind\":\"plugin\",\"slug\":\"a\",\"version\":\"1\"}]", &err)
                  .front().name == "a");
    }

    const fs::path base = fs::temp_directory_path() / "tl_test_marketplace";
    std::error_code ec;
    fs::remove_all(base, ec);

    // --- Extraction stricte ------------------------------------------------------
    {
        std::string err;
        auto refuse = [&](const Files& f, const char* why) {
            const fs::path d = base / "x";
            const bool ok = market::extract_package(make_zip(f), d, &err);
            CHECK(!ok);
            CHECK(!fs::exists(d)); // rien ne reste d'une extraction refusée
            if (ok || !contains(err, why)) std::printf("  attendu « %s », reçu « %s »\n", why, err.c_str());
            fs::remove_all(d, ec);
        };
        refuse({{"../evil.dll", "x"}}, "chemin refusé");
        refuse({{"a/../../evil", "x"}}, "chemin refusé");
        {
            // miniz refuse d'ÉCRIRE un nom absolu : on le forge dans l'archive.
            std::string z = make_zip({{"Xetc/passwd", "x"}});
            for (std::size_t p; (p = z.find("Xetc/passwd")) != std::string::npos;) z[p] = '/';
            const fs::path d = base / "x";
            CHECK(!market::extract_package(z, d, &err) && contains(err, "chemin refusé"));
            CHECK(!fs::exists(d));
        }
        refuse({{"C:/x.dll", "x"}}, "chemin refusé");
        refuse({{"a\\b.dll", "x"}}, "chemin refusé");
        refuse({{"a//b", "x"}}, "chemin refusé");
        refuse({{std::string("bad\x01name"), "x"}}, "chemin refusé");
        refuse({{std::string("\xff\xfe.txt"), "x"}}, "chemin refusé"); // pas de l'UTF-8
        CHECK(!market::extract_package("pas un zip", base / "y", &err));
        CHECK(!market::extract_package(make_zip({}), base / "y", &err));

        Files many;
        for (int i = 0; i < 4100; ++i) many["f" + std::to_string(i)] = "";
        refuse(many, "trop de fichiers");

        // Archive correcte, nom accentué compris.
        const fs::path d = base / "ok";
        CHECK(market::extract_package(make_zip({{"a/b.txt", "1"}, {"donn\xc3\xa9" "es/c.txt", "2"}}),
                                      d, &err));
        CHECK(slurp(d / fs::path(u8"données/c.txt")) == "2");
        // Dossier non vide : refus, et on n'y touche pas.
        CHECK(!market::extract_package(make_zip({{"z", "1"}}), d, &err));
        CHECK(fs::exists(d / "a" / "b.txt"));
    }

    const fs::path interop = fs::path(TL_INTEROP_DIR);
    const Files pluginFiles = read_dir(interop / "plugin");
    const Files modFiles = read_dir(interop / "mod");
    const signing::Roots roots{{root_key()}};
    std::string err;
    const auto rev7 = signing::parse_revocations(slurp(interop / "revocations-7.json"), roots, kNow, -1, &err);
    const auto rev8 = signing::parse_revocations(slurp(interop / "revocations-8.json"), roots, kNow, -1, &err);
    CHECK(rev7 && rev8);

    const market::Entry plugin{Kind::Plugin, "interop", "Interop", "1.0.0", "", {}};
    const market::Entry mod{Kind::Mod, "coolmod", "Cool", "1.0", "", {}};
    const fs::path pluginsDir = base / "plugins", work = base / "staging";

    // --- Installation d'un plugin -------------------------------------------------
    {
        auto r = market::install_from_zip(make_zip(pluginFiles), plugin, pluginsDir, work, roots,
                                          rev7 ? &*rev7 : nullptr, kNow);
        CHECK(r.ok);
        if (!r.ok) std::printf("  %s\n", r.message.c_str());
        CHECK(fs::exists(pluginsDir / "interop" / "plugin.json"));
        CHECK(fs::exists(pluginsDir / "interop" / "signature.json"));
        CHECK(fs::is_empty(work)); // rien ne traîne
        // Réinstallation : remplace proprement.
        fs::create_directories(pluginsDir / "interop" / "ancien");
        r = market::install_from_zip(make_zip(pluginFiles), plugin, pluginsDir, work, roots, nullptr, kNow);
        CHECK(r.ok && !fs::exists(pluginsDir / "interop" / "ancien"));
    }
    auto refused = [&](const std::string& zip, const market::Entry& e, const signing::Roots& rts,
                       const signing::Revocations* revs, const char* why) {
        const fs::path target = base / "refus";
        const auto r = market::install_from_zip(zip, e, target, work, rts, revs, kNow);
        CHECK(!r.ok);
        if (r.ok || !contains(r.message, why))
            std::printf("  attendu « %s », reçu « %s »\n", why, r.message.c_str());
        CHECK(!fs::exists(target / e.slug)); // rien d'installé
        CHECK(!fs::exists(work) || fs::is_empty(work));
    };
    {
        // Version révoquée (liste n°8).
        refused(make_zip(pluginFiles), plugin, roots, rev8 ? &*rev8 : nullptr, "révoquée");
        // Aucune racine dans ce launcher : rien ne s'installe.
        refused(make_zip(pluginFiles), plugin, signing::Roots{}, nullptr, "aucune clé racine");
        // Fichier altéré après signature.
        Files t = pluginFiles;
        t["app/service.exe"] += "porte dérobée";
        refused(make_zip(t), plugin, roots, nullptr, "Signature refusée");
        // Fichier ajouté (une DLL déposée à côté de l'exécutable signé).
        t = pluginFiles;
        t["app/version.dll"] = "MZ";
        refused(make_zip(t), plugin, roots, nullptr, "Signature refusée");
        // Signature retirée.
        t = pluginFiles;
        t.erase("signature.json");
        refused(make_zip(t), plugin, roots, nullptr, "non signée");
        // Annoncé sous un autre nom ou une autre version que ce qui est signé.
        refused(make_zip(pluginFiles), market::Entry{Kind::Plugin, "autre", "", "1.0.0", "", {}},
                roots, nullptr, "Signature refusée");
        refused(make_zip(pluginFiles), market::Entry{Kind::Plugin, "interop", "", "2.0.0", "", {}},
                roots, nullptr, "Signature refusée");
        // Un mod signé ne s'installe pas comme plugin, ni l'inverse.
        refused(make_zip(modFiles), market::Entry{Kind::Plugin, "coolmod", "", "1.0", "", {}},
                roots, nullptr, "Signature refusée");
        refused(make_zip(pluginFiles), market::Entry{Kind::Mod, "interop", "", "1.0.0", "", {}},
                roots, nullptr, "Signature refusée");
        // Identifiant hostile venu du catalogue.
        refused(make_zip(pluginFiles), market::Entry{Kind::Plugin, "../x", "", "1.0.0", "", {}},
                roots, nullptr, "invalide");
    }

    // --- Installation d'un mod ------------------------------------------------------
    {
        const fs::path modsDir = base / "instance" / "mods";
        auto r = market::install_from_zip(make_zip(modFiles), mod, modsDir, work, roots, nullptr, kNow);
        CHECK(r.ok);
        if (!r.ok) std::printf("  %s\n", r.message.c_str());
        CHECK(fs::exists(modsDir / "coolmod-1.0.jar"));
        CHECK(!fs::exists(modsDir / "signature.json"));
        // Jamais d'écrasement d'un mod déjà présent.
        r = market::install_from_zip(make_zip(modFiles), mod, modsDir, work, roots, nullptr, kNow);
        CHECK(!r.ok && contains(r.message, "déjà présent"));
        // Pas de .tl-part oublié.
        int n = 0;
        for (const auto& e : fs::directory_iterator(modsDir)) n += e.is_regular_file() ? 1 : 0;
        CHECK(n == 1);
    }

    fs::remove_all(base, ec);
    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

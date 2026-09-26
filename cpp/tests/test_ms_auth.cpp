// Tests hors reseau du portage MsAuth.cs : base64, DPAPI, encodage de
// formulaire, format d'UUID, caches disque (session + jeton) et logout.
// La chaine Xbox/XSTS/Minecraft n'est pas testable sans compte reel.

#include "ms_auth.hpp"

#include "datastore.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace tl::auth;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

static long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

static std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static void write_file(const fs::path& p, const std::string& s) {
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(s.data(), static_cast<std::streamsize>(s.size()));
}

static std::string b64(const std::string& s) {
    return detail::b64_encode(reinterpret_cast<const unsigned char*>(s.data()),
                              s.size());
}

int main() {
    const fs::path tmp = fs::temp_directory_path() / "tl-ms-auth-test";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);
#ifdef _WIN32
    _putenv_s("TL_DATA_DIR", tmp.string().c_str());
#endif
    const fs::path sessionFile = tmp / "session-cache.json";
    const fs::path tokenFile = tmp / "msauth.json";

    // --- 1. base64 : memes sorties que Convert.ToBase64String (C#) ---
    CHECK_EQ(b64(""), std::string(""));
    CHECK_EQ(b64("f"), std::string("Zg=="));
    CHECK_EQ(b64("fo"), std::string("Zm8="));
    CHECK_EQ(b64("foo"), std::string("Zm9v"));
    CHECK_EQ(b64("Hello"), std::string("SGVsbG8="));
    // Pas de retour a la ligne meme sur une entree longue (CRYPT_STRING_NOCRLF).
    const std::string longIn(300, 'x');
    const std::string longB64 = b64(longIn);
    CHECK(longB64.find('\r') == std::string::npos);
    CHECK(longB64.find('\n') == std::string::npos);
    CHECK_EQ(detail::b64_decode(longB64).value_or(""), longIn);
    CHECK_EQ(detail::b64_decode("SGVsbG8=").value_or(""), std::string("Hello"));
    // Binaire quelconque (le jeton DPAPI n'est pas du texte).
    std::string bin;
    for (int i = 0; i < 256; ++i) bin.push_back(static_cast<char>(i));
    CHECK_EQ(detail::b64_decode(b64(bin)).value_or(""), bin);
    CHECK(!detail::b64_decode("pas du base64 !!!").has_value());

    // --- 2. DPAPI (format ProtectedData.Protect / CurrentUser du C#) ---
    const std::string secret = "eyJhbGciOiJIUzI1NiJ9.jeton-de-test.accents-éàü";
    auto cipher = detail::dpapi_protect_b64(secret);
    CHECK(cipher.has_value());
    if (cipher) {
        CHECK(cipher->find(secret) == std::string::npos); // vraiment chiffre
        CHECK_EQ(detail::dpapi_unprotect_b64(*cipher).value_or(""), secret);
    }
    // Entree qui n'est pas un blob DPAPI -> nullopt (fallback « ancien format »).
    CHECK(!detail::dpapi_unprotect_b64(b64("texte en clair")).has_value());
    CHECK(!detail::dpapi_unprotect_b64("").has_value());

    // --- 3. Encodage de formulaire (FormUrlEncodedContent C#) ---
    CHECK_EQ(detail::url_encode("abcXYZ019-_.~"), std::string("abcXYZ019-_.~"));
    CHECK_EQ(detail::url_encode("service::user.auth.xboxlive.com::MBI_SSL"),
             std::string("service%3A%3Auser.auth.xboxlive.com%3A%3AMBI_SSL"));
    CHECK_EQ(detail::url_encode("urn:ietf:params:oauth:grant-type:device_code"),
             std::string("urn%3Aietf%3Aparams%3Aoauth%3Agrant-type%3Adevice_code"));
    CHECK_EQ(detail::url_encode("a b&c=d"), std::string("a%20b%26c%3Dd"));
    CHECK_EQ(detail::form_body({{"client_id", "00000000402b5328"},
                                {"response_type", "device_code"}}),
             std::string("client_id=00000000402b5328&response_type=device_code"));
    CHECK_EQ(detail::form_body({}), std::string(""));

    // --- 4. FormatUuid ---
    CHECK_EQ(detail::format_uuid("98dd2756bee621bcf8a8e92344183641"),
             std::string("98dd2756-bee6-21bc-f8a8-e92344183641"));
    CHECK_EQ(detail::format_uuid("trop-court"), std::string("trop-court"));
    CHECK_EQ(detail::format_uuid(""), std::string(""));

    // --- 5. Cache de session : aller-retour, jeton chiffre sur disque ---
    AuthSession s;
    s.name = "TeamUN6713";
    s.uuid = "98dd2756-bee6-21bc-f8a8-e92344183641";
    s.token = "jeton-minecraft-de-test";
    detail::save_session_cache(s);
    CHECK(fs::exists(sessionFile));
    {
        const std::string raw = read_file(sessionFile);
        CHECK(raw.find(s.token) == std::string::npos); // jamais en clair
        const json j = json::parse(raw);
        CHECK_EQ(j.value("name", ""), s.name);   // memes cles que le C#
        CHECK_EQ(j.value("uuid", ""), s.uuid);
        CHECK(j.contains("token"));
        CHECK(j.contains("ts"));
    }
    {
        auto loaded = detail::try_load_session_cache();
        CHECK(loaded.has_value());
        if (loaded) {
            CHECK_EQ(loaded->name, s.name);
            CHECK_EQ(loaded->uuid, s.uuid);
            CHECK_EQ(loaded->token, s.token);
        }
    }

    // --- 6. Cache de session : cas de rejet + compat ancien format ---
    auto writeCache = [&](const json& j) { write_file(sessionFile, j.dump()); };

    writeCache({{"name", "X"}, {"uuid", "u"}, {"token", "0"}, {"ts", now_unix()}});
    CHECK(!detail::try_load_session_cache().has_value()); // jeton hors ligne

    writeCache({{"name", "X"}, {"uuid", "u"}, {"token", ""}, {"ts", now_unix()}});
    CHECK(!detail::try_load_session_cache().has_value());

    // --- TTL du fichier : 90 jours (7 776 000 s) ---
    writeCache({{"name", "X"}, {"uuid", "u"}, {"token", "abc"},
                {"ts", now_unix() - 7800000}}); // > 90 j
    CHECK(!detail::try_load_session_cache().has_value());

    writeCache({{"name", "X"}, {"uuid", "u"}, {"token", "abc"},
                {"ts", now_unix() - 7700000}}); // < 90 j
    CHECK(detail::try_load_session_cache().has_value());

    // L'ancien plafond de 7 jours ne doit plus rejeter quoi que ce soit.
    writeCache({{"name", "X"}, {"uuid", "u"}, {"token", "abc"},
                {"ts", now_unix() - 700000}}); // ~8 j
    CHECK(detail::try_load_session_cache().has_value());

    // --- Fraicheur du jeton Minecraft : expiresAt = ts + 23 h ---
    {
        const long long t0 = now_unix();
        writeCache({{"name", "X"}, {"uuid", "u"}, {"token", "abc"}, {"ts", t0}});
        auto fresh = detail::try_load_session_cache();
        CHECK(fresh.has_value());
        if (fresh) {
            CHECK_EQ(fresh->expiresAt, t0 + 82800);
            CHECK(fresh->expiresAt > now_unix()); // frais : utilisable au lancement
        }

        // Jeton de 30 h : le dossier reste lisible (pseudo/uuid affichables)
        // mais le jeton est perime -> renouvellement silencieux attendu.
        const long long old = now_unix() - 108000; // 30 h
        writeCache({{"name", "X"}, {"uuid", "u"}, {"token", "abc"}, {"ts", old}});
        auto stale = detail::try_load_session_cache();
        CHECK(stale.has_value());
        if (stale) {
            CHECK(stale->expiresAt < now_unix());
            CHECK_EQ(stale->name, std::string("X")); // identite conservee
        }
    }

    // Ancien format : jeton en clair, non base64 -> conserve tel quel.
    writeCache({{"name", "Ancien"}, {"uuid", "u"}, {"token", "clair-non-chiffre"},
                {"ts", now_unix()}});
    {
        auto loaded = detail::try_load_session_cache();
        CHECK(loaded.has_value());
        if (loaded) CHECK_EQ(loaded->token, std::string("clair-non-chiffre"));
    }

    // Fichier corrompu / champ manquant -> nullopt, pas d'exception.
    write_file(sessionFile, "{ pas du json");
    CHECK(!detail::try_load_session_cache().has_value());
    writeCache({{"name", "X"}, {"ts", now_unix()}});
    CHECK(!detail::try_load_session_cache().has_value());
    fs::remove(sessionFile, ec);
    CHECK(!detail::try_load_session_cache().has_value());

    // --- 7. Jeton de rafraichissement ---
    const std::string refresh = "M.R3_BAY.refresh-token-de-test";
    detail::save_refresh_token(refresh);
    CHECK(fs::exists(tokenFile));
    CHECK(read_file(tokenFile).find(refresh) == std::string::npos);
    CHECK_EQ(detail::try_load_refresh_token().value_or(""), refresh);
    CHECK(!fs::exists(fs::path(tokenFile).concat(".tmp"))); // tmp consomme
    // Reecriture au-dessus d'un fichier existant (File.Replace C#).
    detail::save_refresh_token("second-jeton");
    CHECK_EQ(detail::try_load_refresh_token().value_or(""),
             std::string("second-jeton"));
    // Jeton vide : le C# n'ecrit rien, on ne doit pas ecraser l'existant.
    detail::save_refresh_token("");
    CHECK_EQ(detail::try_load_refresh_token().value_or(""),
             std::string("second-jeton"));
    // Ancien format en clair (avant chiffrement) : relu tel quel.
    write_file(tokenFile, "  jeton-en-clair\n");
    CHECK_EQ(detail::try_load_refresh_token().value_or(""),
             std::string("jeton-en-clair"));

    // --- 7bis. Renouvellement au démarrage sans jeton conservé ---
    // Aucun refresh token sur disque : startup_refresh() ne doit rien faire de
    // visible (pas de modale, pas d'erreur, état laissé à Idle).
    fs::remove(tokenFile, ec);
    tl::DataStore::settings.accountMode = "microsoft";
    startup_refresh();
    for (int i = 0; i < 100 && get_state() != AuthState::Idle; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    CHECK_EQ(static_cast<int>(get_state()), static_cast<int>(AuthState::Idle));
    CHECK(!get_login_info().has_value());
    CHECK(!fs::exists(tokenFile));
    stop();

    // En mode hors ligne, aucun thread ne doit être lancé du tout.
    tl::DataStore::settings.accountMode = "offline";
    startup_refresh();
    CHECK_EQ(static_cast<int>(get_state()), static_cast<int>(AuthState::Idle));
    stop();
    tl::DataStore::settings.accountMode = "microsoft";

    // --- 8. API publique : get_session / has_session / logout ---
    detail::save_session_cache(s);
    CHECK(has_session());
    {
        auto pub = get_session();
        CHECK(pub.has_value());
        if (pub) CHECK_EQ(pub->name, s.name);
    }
    CHECK_EQ(static_cast<int>(get_state()), static_cast<int>(AuthState::Idle));
    CHECK(!get_login_info().has_value()); // rien a afficher hors connexion

    logout();
    CHECK(!fs::exists(tokenFile));
    CHECK(!fs::exists(sessionFile)); // divergence assumee vs C# (qui gardait le cache)
    CHECK(!has_session());
    CHECK(!get_session().has_value());

    // --- 9. Reseau (TL_TEST_NET=1) : vrai device code chez Microsoft ---
    if (std::getenv("TL_TEST_NET")) {
        std::printf("INFO test reseau : demande de device code...\n");
        _putenv_s("TL_NO_BROWSER", "1"); // ne pas ouvrir de navigateur
        login_start(/*force=*/true);

        AuthState st = AuthState::Idle;
        for (int i = 0; i < 300; ++i) { // 30 s max
            st = get_state();
            if (st == AuthState::WaitingCode || st == AuthState::Error) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        CHECK_EQ(static_cast<int>(st), static_cast<int>(AuthState::WaitingCode));

        auto info = get_login_info();
        CHECK(info.has_value());
        if (info) {
            std::printf("INFO code=%s url=%s\n", info->userCode.c_str(),
                        info->verificationUrl.c_str());
            CHECK(!info->userCode.empty());
            CHECK(info->verificationUrl.rfind("http", 0) == 0);
        }

        // Annulation : le thread doit se terminer et repasser Idle.
        cancel_login();
        stop();
        CHECK_EQ(static_cast<int>(get_state()), static_cast<int>(AuthState::Idle));
        CHECK(!has_session());
        // Aucun jeton conserve tant que le joueur n'a pas valide le code.
        CHECK(!fs::exists(tokenFile));
    } else {
        std::printf("INFO test reseau saute (TL_TEST_NET non defini)\n");
    }

    stop(); // sans thread en cours : doit etre sans effet
    fs::remove_all(tmp, ec);

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

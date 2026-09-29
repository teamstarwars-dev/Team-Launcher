#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#endif

#include "ms_auth.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line
#include "http_win.hpp"
#include "proc.hpp" // open_detached (POSIX) ; vide sous Windows
#include "obf.hpp" // endpoints et ID client obfusses (S3)

#include <nlohmann/json.hpp>

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using nlohmann::json;
using namespace std::chrono_literals;

namespace tl::auth {

namespace {

// L'ID client reste publique en `const char* const` (ms_auth.hpp, hors du
// perimetre de modification ici) : la valeur obfusquee est donc dechiffree
// une seule fois dans une string statique, dont c_str() reste valable tant
// que le processus tourne. Seuls les lecteurs de kDefaultClientId sont dans
// ce TU, et tous passent par une fonction definie ici (init garantie avant).
const std::string& client_id() {
    static const std::string v = TL_OBF("00000000402b5328");
    return v;
}

} // namespace

const char* const kDefaultClientId = client_id().c_str();

// Toutes les aides internes vivent dans `detail` : le sous-ensemble declare
// dans ms_auth.hpp est teste par tests/test_ms_auth.cpp, le reste n'est pas
// declare ailleurs (equivalent d'une portee fichier, LTO + /OPT:REF nettoient).
namespace detail {

// Endpoints Microsoft : obfusses (chaines d'infrastructure, jamais en clair
// dans .rdata). `constexpr` impossible : TL_OBF dechiffre a l'execution.
// Usages verifies : conversion implicite en std::string vers post_form(), ni
// contexte constexpr ni comparaison de pointeurs.
const std::string kConnectEndpoint =
    TL_OBF("https://login.live.com/oauth20_connect.srf");
const std::string kTokenEndpoint =
    TL_OBF("https://login.live.com/oauth20_token.srf");
constexpr const char* kAuthScope = "service::user.auth.xboxlive.com::MBI_SSL";
// Duree de conservation du cache de session sur disque. Le C# gardait 7 jours
// (604 800 s) alors que son commentaire annoncait 24 h. On garde desormais le
// dossier 90 jours : ce n'est PAS la duree de validite du jeton Minecraft, mais
// celle de l'enregistrement local (pseudo, uuid, dernier jeton connu).
constexpr long long kSessionTtlSec = 7776000; // 90 jours

// Le jeton Minecraft delivre par login_with_xbox vit ~24 h cote Mojang. On le
// considere perime un peu avant (marge d'1 h) pour ne jamais lancer le jeu avec
// un jeton mort : au-dela, il est regenere en silence via le refresh token.
constexpr long long kMcTokenFreshSec = 82800; // 23 h

// Annulation propre : distincte d'une erreur (pas de modale rouge).
struct Cancelled {};

fs::path token_file() { return DataStore::dir() / "msauth.json"; }
fs::path session_file() { return DataStore::dir() / "session-cache.json"; }

void auth_log(const std::string& s) { log_line("[Auth] " + s); }

long long now_unix() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string truncate(const std::string& s, size_t n) {
    return s.size() > n ? s.substr(0, n) : s;
}

// ---------------------------------------------------------------------------
// base64 + DPAPI : deplaces dans secrets.cpp (tl::secrets) pour partage avec
// DataStore ; re-exportes par ms_auth.hpp (detail::*).
// ---------------------------------------------------------------------------

// RAII : le secret est ecrase sur TOUS les chemins de sortie (return/throw).
struct WipeGuard {
    std::string& s;
    ~WipeGuard() { secure_wipe(s); }
};

// ---------------------------------------------------------------------------
// Fichiers
// ---------------------------------------------------------------------------

std::optional<std::string> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool write_all(const fs::path& p, const std::string& data) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(out);
}

// Ecriture atomique : le lecteur (autre instance, antivirus) ne voit jamais
// un fichier tronqué. Si le rename échoue (destination verrouillée), on
// retente après suppression — jamais de .tmp orphelin en cas de succès.
bool write_atomic(const fs::path& dst, const std::string& data) {
    const fs::path tmp = fs::path(dst).concat(".tmp");
    if (!write_all(tmp, data)) return false;
    std::error_code ec;
    fs::rename(tmp, dst, ec);
    if (ec) {
        fs::remove(dst, ec);
        fs::rename(tmp, dst, ec);
    }
    return !ec;
}

// ---------------------------------------------------------------------------
// HTTP : formulaire + JSON, messages d'erreur fideles au C#
// ---------------------------------------------------------------------------

std::string url_encode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size() * 3 / 2);
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~')
            out.push_back(static_cast<char>(ch));
        else {
            out.push_back('%');
            out.push_back(hex[ch >> 4]);
            out.push_back(hex[ch & 0xF]);
        }
    }
    return out;
}

using Form = std::vector<std::pair<std::string, std::string>>;

std::string form_body(const Form& f) {
    std::string out;
    for (const auto& [k, v] : f) {
        if (!out.empty()) out.push_back('&');
        out += url_encode(k);
        out.push_back('=');
        out += url_encode(v);
    }
    return out;
}

struct Cancel {
    std::atomic<bool>* ext = nullptr;
};

bool cancelled(const Cancel& c);              // defini apres ctx()
void throw_if_cancelled(const Cancel& c) {
    if (cancelled(c)) throw Cancelled{};
}

json parse_body(const std::optional<http::Response>& r, const std::string& url) {
    if (!r) throw std::runtime_error("Échec réseau vers " + url +
                                     "\nVérifie ta connexion Internet.");
    try {
        return json::parse(r->body);
    } catch (const json::exception&) {
        if (r->status == 401 && url.find("user.auth.xboxlive.com") != std::string::npos)
            throw std::runtime_error(
                "Xbox Live a refusé le jeton Microsoft (HTTP 401).\n"
                "Relance la connexion : si l'erreur persiste, déconnecte-toi puis\n"
                "reconnecte-toi sur account.microsoft.com avant de réessayer.");
        throw std::runtime_error("HTTP " + std::to_string(r->status) + " depuis " +
                                 url + "\nRéponse : " + truncate(r->body, 400));
    }
}

json post_form(const std::string& url, const Form& f, const Cancel& c) {
    throw_if_cancelled(c);
    auto r = http::post_string(url, form_body(f),
                               "application/x-www-form-urlencoded", {}, c.ext);
    throw_if_cancelled(c);
    return parse_body(r, url);
}

json post_json(const std::string& url, const json& body, const Cancel& c) {
    throw_if_cancelled(c);
    // En-tetes exiges par les services Xbox Live (cf. PostJsonAsync C#).
    std::string headers;
    if (url.find("xboxlive.com") != std::string::npos)
        headers = "x-xbl-contract-version: 1\r\nAccept: application/json";
    auto r = http::post_string(url, body.dump(), "application/json", headers, c.ext);
    throw_if_cancelled(c);
    return parse_body(r, url);
}

// Champ obligatoire : message lisible plutot qu'une exception nlohmann brute.
const json& field(const json& j, const char* key, const char* what) {
    auto it = j.find(key);
    if (it == j.end())
        throw std::runtime_error(std::string("Réponse inattendue de ") + what +
                                 " : champ « " + key + " » absent.\n" +
                                 truncate(j.dump(), 300));
    return *it;
}

std::string field_str(const json& j, const char* key, const char* what) {
    const json& v = field(j, key, what);
    if (!v.is_string())
        throw std::runtime_error(std::string("Réponse inattendue de ") + what +
                                 " : champ « " + key + " » non textuel.");
    return v.get<std::string>();
}

// ---------------------------------------------------------------------------
// Etat partage
// ---------------------------------------------------------------------------

struct Ctx {
    std::mutex m;
    std::condition_variable cv;
    AuthState state = AuthState::Idle;
    LoginInfo info;
    std::optional<AuthSession> session;
    std::string error;
    bool running = false;
    bool diskChecked = false;
    // Renouvellement silencieux : n'expose aucun etat a l'UI (set_state est
    // ignore), la modale de connexion ne doit jamais s'ouvrir au demarrage.
    bool quiet = false;
    std::atomic<bool> cancelReq{false};
    std::thread th;
};

Ctx& ctx() {
    static Ctx c;
    return c;
}

bool cancelled(const Cancel& c) {
    return ctx().cancelReq.load(std::memory_order_relaxed) ||
           (c.ext && c.ext->load(std::memory_order_relaxed));
}

void set_state(AuthState s, const std::string& msg) {
    std::lock_guard<std::mutex> lk(ctx().m);
    if (ctx().quiet) return;
    ctx().state = s;
    ctx().info.message = msg;
}

void sleep_cancellable(int seconds, const Cancel& c) {
    for (int i = 0; i < seconds * 5; ++i) {
        throw_if_cancelled(c);
        std::this_thread::sleep_for(200ms);
    }
    throw_if_cancelled(c);
}

// ---------------------------------------------------------------------------
// Cache de session disque (session-cache.json, format C#)
// ---------------------------------------------------------------------------

std::optional<AuthSession> try_load_session_cache() {
    try {
        auto raw = read_all(session_file());
        if (!raw) {
            auth_log("Cache disque : introuvable.");
            return std::nullopt;
        }
        WipeGuard rawGuard{*raw}; // contenu chiffré : écrasé dès la lecture
        json root = json::parse(*raw);

        long long ts = 0;
        if (auto it = root.find("ts"); it != root.end() && it->is_number_integer()) {
            ts = it->get<long long>();
            const long long age = now_unix() - ts;
            if (age > kSessionTtlSec) {
                auth_log("Cache disque : expiré (" + std::to_string(age) + "s).");
                return std::nullopt;
            }
        }

        AuthSession s;
        s.name = field_str(root, "name", "cache de session");
        s.uuid = field_str(root, "uuid", "cache de session");
        std::string tokenRaw = field_str(root, "token", "cache de session");
        WipeGuard tokenGuard{tokenRaw};
        if (tokenRaw.empty() || tokenRaw == "0") {
            auth_log("Cache disque : token vide.");
            return std::nullopt;
        }
        // Chiffre DPAPI, ou ancien format en clair.
        s.token = dpapi_unprotect_b64(tokenRaw).value_or(tokenRaw);
        // expiresAt = moment ou le JETON MINECRAFT doit etre regenere (~23 h),
        // a ne pas confondre avec kSessionTtlSec (conservation du fichier).
        s.expiresAt = ts + kMcTokenFreshSec;

        auth_log("Cache disque : OK (nom=" + s.name + ", jeton " +
                 (now_unix() < s.expiresAt ? "frais" : "à renouveler") + ").");
        return s;
    } catch (const std::exception& ex) {
        auth_log(std::string("Cache disque : erreur ") + ex.what());
        return std::nullopt;
    }
}

void save_session_cache(const AuthSession& s) {
    try {
        // SÉCURITÉ : si DPAPI est indisponible, on NE PERSISTE RIEN. L'ancien
        // repli « jeton en clair » du C# exposait un bearer token (~24 h
        // d'accès complet au compte) à tout processus lisant le profil. La
        // session reste utilisable en mémoire jusqu'à la fermeture ; seule la
        // LECTURE du format clair est conservée (compat fichiers v5).
        auto cipher = dpapi_protect_b64(s.token);
        if (!cipher) {
            auth_log("SaveSessionCache : DPAPI indisponible, session non "
                     "conservée (mémoire seule).");
            return;
        }
        json obj = {{"name", s.name},
                    {"uuid", s.uuid},
                    {"token", *cipher},
                    {"ts", now_unix()}};
        if (!write_atomic(session_file(), obj.dump()))
            auth_log("SaveSessionCache : écriture impossible.");
    } catch (const std::exception& ex) {
        auth_log(std::string("SaveSessionCache : ") + ex.what());
    }
}

// ---------------------------------------------------------------------------
// Jeton de rafraichissement (msauth.json)
// ---------------------------------------------------------------------------

std::optional<std::string> try_load_refresh_token() {
    auto raw = read_all(token_file());
    if (!raw) return std::nullopt;
    WipeGuard rawGuard{*raw};
    // trim
    const auto b = raw->find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return std::nullopt;
    const auto e = raw->find_last_not_of(" \t\r\n");
    std::string t = raw->substr(b, e - b + 1);
    WipeGuard tGuard{t};
    if (t.empty()) return std::nullopt;
    return dpapi_unprotect_b64(t).value_or(t); // ancien format en clair
}

void save_refresh_token(const std::string& token) {
    if (token.empty()) return;
    auto cipher = dpapi_protect_b64(token);
    if (!cipher) {
        auth_log("SaveRefreshToken : DPAPI indisponible, jeton non conservé.");
        return;
    }
    if (!write_atomic(token_file(), *cipher))
        auth_log("SaveRefreshToken : écriture impossible.");
}

void delete_refresh_token() {
    std::error_code ec;
    fs::remove(token_file(), ec);
}

// ---------------------------------------------------------------------------
// Chaine Xbox Live -> XSTS -> Minecraft
// ---------------------------------------------------------------------------

std::string format_uuid(const std::string& hex) {
    if (hex.size() != 32) return hex;
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) +
           "-" + hex.substr(16, 4) + "-" + hex.substr(20);
}

bool contains_ci(const std::string& hay, const std::string& needle) {
    auto lower = [](std::string s) {
        for (auto& ch : s) ch = static_cast<char>(std::tolower((unsigned char)ch));
        return s;
    };
    return lower(hay).find(lower(needle)) != std::string::npos;
}

// Skin officiel : textures du profil (base64) -> skins/<pseudo>.png. Silencieux.
void save_official_skin(const std::string& name, const json& profile,
                        const Cancel& c) {
    try {
        auto props = profile.find("properties");
        if (props == profile.end() || !props->is_array()) {
            auth_log("Pas de texture de skin dans le profil.");
            return;
        }
        for (const auto& p : *props) {
            // Quirk C# : une entree SANS champ « name » n'est pas ignoree.
            auto pn = p.find("name");
            if (pn != p.end() && pn->is_string() && pn->get<std::string>() != "textures")
                continue;

            auto decoded = b64_decode(field_str(p, "value", "profil"));
            if (!decoded) continue;
            json tex = json::parse(*decoded);
            const std::string url = field_str(
                field(field(tex, "textures", "textures"), "SKIN", "textures"), "url",
                "textures");

            auto png = http::get_response(url, {}, c.ext);
            if (!png || png->status != 200 || png->body.empty()) {
                auth_log("Récupération du skin impossible : HTTP " +
                         std::to_string(png ? png->status : 0));
                return;
            }
            std::error_code ec;
            fs::create_directories(DataStore::skinsDir(), ec);
            // Le pseudo vient du serveur : jamais de chemin brut vers le disque.
            write_all(DataStore::skinsDir() / (sanitize_file_stem(name) + ".png"),
                      png->body);
            auth_log("Skin officiel enregistré (" + std::to_string(png->body.size()) +
                     " octets).");
            return;
        }
        auth_log("Pas de texture de skin dans le profil.");
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& ex) {
        auth_log(std::string("Récupération du skin impossible : ") + ex.what());
    }
}

AuthSession xbox_to_minecraft(const std::string& msAccessToken, const Cancel& c) {
    // 3. Xbox Live — les jetons du flux legacy login.live.com passent SANS « d= ».
    set_state(AuthState::Chaining, "Connexion à Xbox Live...");
    auth_log("Xbox Live : envoi du jeton Microsoft (" +
             std::to_string(msAccessToken.size()) + " car.)");
    const json xbl = post_json(
        "https://user.auth.xboxlive.com/user/authenticate",
        {{"Properties",
          {{"AuthMethod", "RPS"},
           {"SiteName", "user.auth.xboxlive.com"},
           {"RpsTicket", msAccessToken}}},
         {"RelyingParty", "http://auth.xboxlive.com"},
         {"TokenType", "JWT"}},
        c);

    const json& xui = field(field(xbl, "DisplayClaims", "Xbox Live"), "xui", "Xbox Live");
    if (!xui.is_array() || xui.empty())
        throw std::runtime_error("Réponse inattendue de Xbox Live : aucun profil.");
    const std::string uhs = field_str(xui[0], "uhs", "Xbox Live");
    std::string xblToken = field_str(xbl, "Token", "Xbox Live");
    WipeGuard xblGuard{xblToken};
    auth_log("Xbox Live : OK");

    // 4. XSTS
    set_state(AuthState::Chaining, "Autorisation XSTS...");
    const json xsts =
        post_json("https://xsts.auth.xboxlive.com/xsts/authorize",
                  {{"Properties",
                    {{"SandboxId", "RETAIL"}, {"UserTokens", json::array({xblToken})}}},
                   {"RelyingParty", "rp://api.minecraftservices.com/"},
                   {"TokenType", "JWT"}},
                  c);
    std::string xstsToken = field_str(xsts, "Token", "XSTS");
    WipeGuard xstsGuard{xstsToken};
    auth_log("XSTS : OK");

    // 5. Jeton Minecraft
    set_state(AuthState::Chaining, "Obtention du jeton Minecraft...");
    const json mcLogin =
        post_json("https://api.minecraftservices.com/authentication/login_with_xbox",
                  {{"identityToken", "XBL3.0 x=" + uhs + ";" + xstsToken}}, c);
    auto atIt = mcLogin.find("access_token");
    if (atIt == mcLogin.end() || !atIt->is_string()) {
        // Journal tronqué comme le message : un corps d'erreur ne doit jamais
        // déverser un jeton complet dans launcher.log.
        const std::string brut = truncate(mcLogin.dump(), 300);
        auth_log("login_with_xbox réponse inattendue : " + brut);
        if (contains_ci(brut, "Invalid app registration"))
            throw std::runtime_error(
                "L'ID client Azure du launcher n'est pas encore validé par Mojang.\n\n"
                "Depuis 2025, Mojang exige que chaque application soit approuvée "
                "manuellement\n"
                "(formulaire officiel : https://aka.ms/mce-reviewappid — délai ~3-4 "
                "semaines).\n"
                "La connexion fonctionnera dès que l'approbation sera accordée ; ce "
                "n'est pas un bug du launcher.");
        throw std::runtime_error("Réponse inattendue des serveurs Minecraft :\n" +
                                 truncate(brut, 300));
    }
    const std::string mcToken = atIt->get<std::string>();
    auth_log("Jeton Minecraft : OK");

    // 6. Profil
    set_state(AuthState::Chaining, "Lecture du profil Minecraft...");
    throw_if_cancelled(c);
    auto profResp = http::get_response("https://api.minecraftservices.com/minecraft/profile",
                                       "Authorization: Bearer " + mcToken, c.ext);
    throw_if_cancelled(c);
    if (!profResp)
        throw std::runtime_error(
            "Échec réseau vers api.minecraftservices.com (profil Minecraft).");
    auth_log("Profil : HTTP " + std::to_string(profResp->status));
    if (profResp->status == 404)
        throw std::runtime_error(
            "Ton compte Microsoft ne possède pas encore de profil Minecraft Java.\n"
            "Lance une fois Minecraft (même en solo) depuis le launcher officiel\n"
            "pour créer le profil, puis retente.");

    json prof;
    try {
        prof = json::parse(profResp->body);
    } catch (const json::exception&) {
        throw std::runtime_error("Réponse profil invalide : " +
                                 truncate(profResp->body, 400));
    }
    auto nameIt = prof.find("name");
    if (nameIt == prof.end() || !nameIt->is_string())
        throw std::runtime_error("Ce compte ne possède pas Minecraft Java.");
    const std::string name = nameIt->get<std::string>();
    const std::string rawId = field_str(prof, "id", "profil Minecraft");
    auth_log("Profil trouvé : " + name);

    save_official_skin(name, prof, c);

    AuthSession s;
    s.name = name;
    s.uuid = format_uuid(rawId);
    s.token = mcToken;
    s.expiresAt = now_unix() + kMcTokenFreshSec;
    return s;
}

// ---------------------------------------------------------------------------
// Flux complet
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Garde-fous durs (pseudo reseau -> disque, URL serveur -> navigateur)
// ---------------------------------------------------------------------------

// Pseudo serveur -> tige de fichier : seuls [A-Za-z0-9_-] survivent (64 car.
// max, « skin » sinon). Les pseudos Minecraft sont déjà contraints côté
// serveur, mais le nom de fichier ne doit jamais dépendre de la confiance.
std::string sanitize_file_stem(const std::string& s) {
    std::string out;
    for (unsigned char ch : s) {
        if (std::isalnum(ch) || ch == '_' || ch == '-') {
            if (out.size() >= 64) break;
            out.push_back(static_cast<char>(ch));
        }
    }
    return out.empty() ? "skin" : out;
}

// L'URL de validation vient du JSON Microsoft : on ne l'ouvre que si c'est du
// https vers un hôte Microsoft/Xbox connu. Tout le reste (http, file:, Pseudo...,
// hôte inconnu) est refusé et journalisé.
bool browser_url_allowed(const std::string& url) {
    std::string head = url.substr(0, 8);
    for (auto& ch : head) ch = static_cast<char>(std::tolower((unsigned char)ch));
    if (head != "https://") return false;
    std::string host = url.substr(8);
    for (const char sep : {'/', '?', '#'}) {
        const auto p = host.find(sep);
        if (p != std::string::npos) host.resize(p);
    }
    for (auto& ch : host) ch = static_cast<char>(std::tolower((unsigned char)ch));
    if (host.empty()) return false;
    // Hôtes exacts du flux device code + suffixes de confiance (avec frontière
    // de point : « fakemicrosoft.com » ne passe pas « .microsoft.com »).
    if (host == "login.live.com" || host == "login.microsoftonline.com" ||
        host == "microsoft.com" || host == "account.microsoft.com")
        return true;
    for (const char* suf :
         {".live.com", ".microsoft.com", ".microsoftonline.com", ".xbox.com"}) {
        const size_t n = std::strlen(suf);
        if (host.size() > n && host.compare(host.size() - n, n, suf) == 0)
            return true;
    }
    return false;
}

void open_browser(const std::string& uri) {
    if (std::getenv("TL_NO_BROWSER")) return; // tests
    if (!browser_url_allowed(uri)) {
        auth_log("Navigateur : URL de validation inattendue, ouverture "
                 "refusée (" +
                 truncate(uri, 120) + ").");
        return;
    }
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, uri.c_str(), -1, nullptr, 0);
    if (n <= 1) return;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, uri.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    proc::open_detached(uri);
#endif
}

// Renouvellement silencieux. Microsoft fait tourner le refresh token a chaque
// usage : on reecrit celui qu'il renvoie, ce qui fait glisser la fenetre de
// validite tant que l'utilisateur se sert du launcher.
//
// ATTENTION : l'expiration finale ne depend PAS de nous. Microsoft peut
// invalider le refresh token a tout moment (inactivite prolongee, changement
// ou reinitialisation du mot de passe, revocation depuis account.microsoft.com,
// exigence de nouvelle authentification forte...). Ce cas n'est donc pas une
// anomalie : on supprime le jeton mort et on retombe proprement sur l'ecran de
// connexion par code d'appareil — jamais de plantage ni d'echec muet.
std::optional<std::string> try_refresh(std::string refreshToken, const Cancel& c) {
    WipeGuard guard{refreshToken}; // écrasé sur tous les chemins de sortie
    try {
        const json tok = post_form(kTokenEndpoint,
                                   {{"client_id", kDefaultClientId},
                                    {"grant_type", "refresh_token"},
                                    {"refresh_token", refreshToken},
                                    {"scope", kAuthScope}},
                                   c);
        const std::string access = field_str(tok, "access_token", "Microsoft");
        save_refresh_token(tok.value("refresh_token", std::string{}));
        auth_log("Jeton Microsoft renouvelé silencieusement.");
        return access;
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& ex) {
        // Refus de Microsoft : le jeton conserve ne vaut plus rien.
        auth_log(std::string("Renouvellement refusé par Microsoft (reconnexion "
                             "nécessaire) : ") +
                 ex.what());
        delete_refresh_token();
        return std::nullopt;
    }
}

AuthSession run_flow(bool force, const Cancel& c) {
    std::string msAccessToken;
    WipeGuard msGuard{msAccessToken}; // jeton Microsoft : écrasé après la chaîne

    // force : on saute le jeton conserve pour permettre « changer de compte ».
    if (!force) {
        if (auto rt = try_load_refresh_token()) {
            auto renewed = try_refresh(*rt, c);
            secure_wipe(*rt); // la copie transmise est déjà protégée par garde
            if (renewed) {
                msAccessToken = *renewed;
                secure_wipe(*renewed);
            }
        }
    }

    if (msAccessToken.empty()) {
        // 1. Demande de code appareil (endpoint Live Connect historique)
        set_state(AuthState::RequestingCode, "Demande du code à Microsoft...");
        const json dc = post_form(kConnectEndpoint,
                                  {{"client_id", kDefaultClientId},
                                   {"response_type", "device_code"},
                                   {"scope", kAuthScope}},
                                  c);
        auto codeIt = dc.find("user_code");
        if (codeIt == dc.end() || !codeIt->is_string()) {
            const std::string detail = dc.value("error_description", dc.dump());
            throw std::runtime_error(
                "Microsoft a refusé la demande de connexion.\nDétail technique : " +
                detail);
        }
        const std::string userCode = codeIt->get<std::string>();
        // Lien avec le code pre-rempli si Microsoft le fournit, sinon page standard.
        std::string verifyUri = dc.value("verification_uri_complete", std::string{});
        if (verifyUri.empty()) verifyUri = field_str(dc, "verification_uri", "Microsoft");
        std::string deviceCode = field_str(dc, "device_code", "Microsoft");
        WipeGuard deviceGuard{deviceCode};
        int interval = 5;

        open_browser(verifyUri);
        {
            std::lock_guard<std::mutex> lk(ctx().m);
            ctx().info.userCode = userCode;
            ctx().info.verificationUrl = verifyUri;
        }
        set_state(AuthState::WaitingCode,
                  "Entre ce code sur la page Microsoft qui vient de s'ouvrir.");
        // Le code reste affiché à l'écran et au presse-papiers : inutile de
        // l'écrire aussi dans le journal (qui finit souvent copié-collé).
        auth_log("Code appareil obtenu, en attente de validation.");

        // 2. Attente de la validation par le joueur
        for (;;) {
            sleep_cancellable(interval, c);
            const json tok = post_form(kTokenEndpoint,
                                       {{"client_id", kDefaultClientId},
                                        {"grant_type",
                                         "urn:ietf:params:oauth:grant-type:device_code"},
                                        {"device_code", deviceCode}},
                                       c);
            if (auto at = tok.find("access_token");
                at != tok.end() && at->is_string()) {
                msAccessToken = at->get<std::string>();
                save_refresh_token(tok.value("refresh_token", std::string{}));
                break;
            }
            const std::string err = tok.value("error", std::string{});
            if (err == "authorization_pending") continue;
            if (err == "slow_down") {
                interval += 5;
                continue;
            }
            if (err == "authorization_declined")
                throw std::runtime_error("Connexion refusée depuis la page Microsoft.");
            if (err == "expired_token")
                throw std::runtime_error(
                    "Le code a expiré avant d'être validé. Relancez la connexion.");
            throw std::runtime_error("Connexion refusée : " + err);
        }
    }

    return xbox_to_minecraft(msAccessToken, c);
}

// Session exploitable (pseudo/uuid connus, jeton non hors-ligne).
bool session_usable(const std::optional<AuthSession>& s) {
    return s && !s->token.empty() && s->token != "0";
}

// Session dont le jeton Minecraft est encore accepte par Mojang (< ~23 h).
// Une session « utilisable mais plus fraiche » sert a afficher le pseudo, pas
// a lancer le jeu : il faut d'abord regenerer le jeton.
bool session_fresh(const std::optional<AuthSession>& s) {
    return session_usable(s) && now_unix() < s->expiresAt;
}

void remember(const AuthSession& s) {
    std::lock_guard<std::mutex> lk(ctx().m);
    ctx().session = s;
    ctx().diskChecked = true;
}

// Pourquoi un renouvellement silencieux n'a pas abouti. La distinction compte :
// seul `RefreshRefused` justifie de redemander une connexion a l'utilisateur.
enum class RenewFail {
    None,           // succes
    NoToken,        // aucun refresh token conserve (jamais connecte / deconnecte)
    RefreshRefused, // Microsoft a refuse le refresh token -> reconnexion requise
    Transient       // Xbox/XSTS/Mojang indisponible, reseau coupe -> on reessaiera
};

// Regenere le jeton Minecraft sans interaction :
//   refresh token -> Xbox Live -> XSTS -> login_with_xbox -> profil.
// La session en cache n'est jamais detruite par un echec passager.
std::optional<AuthSession> silent_renew(const Cancel& c, RenewFail* why = nullptr) {
    auto set = [&](RenewFail f) {
        if (why) *why = f;
    };
    set(RenewFail::None);

    auto rt = try_load_refresh_token();
    if (!rt) {
        set(RenewFail::NoToken);
        return std::nullopt;
    }
    // try_refresh() supprime le jeton s'il est refuse : c'est le seul cas ou la
    // session est reellement finie cote Microsoft.
    auto access = try_refresh(*rt, c);
    secure_wipe(*rt);
    if (!access) {
        set(RenewFail::RefreshRefused);
        return std::nullopt;
    }
    try {
        AuthSession s = xbox_to_minecraft(*access, c);
        save_session_cache(s);
        remember(s);
        auth_log("Jeton Minecraft régénéré en silence (aucune reconnexion).");
        return s;
    } catch (const Cancelled&) {
        throw;
    } catch (const std::exception& ex) {
        // Le refresh token vient d'etre accepte et renouvele : l'echec est en
        // aval (Xbox Live, XSTS ou api.minecraftservices.com). C'est passager,
        // on garde la session et le jeton conserve.
        set(RenewFail::Transient);
        auth_log(std::string("Régénération du jeton Minecraft impossible pour "
                             "l'instant (jeton Microsoft conservé) : ") +
                 ex.what());
        return std::nullopt;
    }
}

// Retourne la session, ou leve. Met a jour le cache memoire.
AuthSession do_login(bool force, const Cancel& c) {
    if (!force) {
        // 1. Cache memoire encore frais : instantane.
        {
            std::lock_guard<std::mutex> lk(ctx().m);
            if (session_fresh(ctx().session)) {
                auth_log("Session cache mémoire utilisée (instantané).");
                return *ctx().session;
            }
        }
        // 2. Cache disque (conserve 90 j) : utilisable tel quel si le jeton
        //    Minecraft n'a pas 23 h.
        auto disk = try_load_session_cache();
        if (session_fresh(disk)) {
            auth_log("Session cache disque utilisée (rapide).");
            remember(*disk);
            return *disk;
        }
        // 3. Jeton perime (ou absent) mais compte connu : renouvellement
        //    silencieux. C'est le cas courant a partir du 2e jour d'usage.
        RenewFail why = RenewFail::None;
        if (auto renewed = silent_renew(c, &why)) return *renewed;

        // 4. Echec en aval (Mojang indisponible, reseau coupe) : inutile de
        //    demander une reconnexion, le compte est toujours valide.
        if (why == RenewFail::Transient)
            throw std::runtime_error(
                "Les serveurs Minecraft n'ont pas répondu.\n"
                "Ta session est conservée : réessaie dans un moment.");

        // 5. Microsoft a refuse le jeton conserve (inactivite prolongee,
        //    mot de passe change, revocation...) : reconnexion par code.
        if (session_usable(disk))
            auth_log("Session expirée côté Microsoft : reconnexion demandée.");
    }

    AuthSession s = run_flow(force, c);
    save_session_cache(s);
    remember(s);
    return s;
}

// Corps commun au thread de fond et a login_blocking (execution en ligne).
// Suppose running==true deja pose par l'appelant.
void run_worker(bool force, std::atomic<bool>* ext) {
    std::optional<AuthSession> s;
    std::string err;
    bool wasCancelled = false;
    try {
        s = do_login(force, Cancel{ext});
    } catch (const Cancelled&) {
        wasCancelled = true;
    } catch (const std::exception& ex) {
        err = ex.what();
    } catch (...) {
        err = "Erreur inconnue pendant la connexion Microsoft.";
    }

    std::lock_guard<std::mutex> lk(ctx().m);
    ctx().running = false;
    if (s) {
        ctx().state = AuthState::Done;
        ctx().error.clear();
        ctx().info.message = "Connecté en tant que " + s->name + ".";
        auth_log("Connexion réussie : " + s->name);
    } else if (wasCancelled) {
        ctx().state = AuthState::Idle;
        ctx().error = "Connexion Microsoft annulée.";
        ctx().info = {};
        auth_log("Connexion annulée.");
    } else {
        ctx().state = AuthState::Error;
        ctx().error = err;
        ctx().info.message = err;
        auth_log("Échec auth Microsoft : " + err);
    }
    ctx().cv.notify_all();
}

// Prepare l'etat pour un nouveau flux. Retourne false si un flux tourne deja.
bool begin_flow() {
    Ctx& c = ctx();
    if (c.running) return false;
    if (c.th.joinable()) c.th.join(); // thread precedent termine
    c.running = true;
    c.cancelReq.store(false);
    c.error.clear();
    c.info = LoginInfo{};
    c.state = AuthState::RequestingCode;
    return true;
}

} // namespace detail

using namespace detail;

// ---------------------------------------------------------------------------
// API publique
// ---------------------------------------------------------------------------

void login_start(bool force) {
    Ctx& c = ctx();
    std::lock_guard<std::mutex> lk(c.m);
    if (!begin_flow()) return;
    c.th = std::thread([force] { run_worker(force, nullptr); });
}

bool login_blocking(std::atomic<bool>* cancel, std::string* errOut) {
    Ctx& c = ctx();
    std::unique_lock<std::mutex> lk(c.m);

    // Fraicheur exigee : lancer le jeu avec un jeton de plus de 23 h le ferait
    // demarrer « non authentifie » (impossible de rejoindre un serveur online).
    if (session_fresh(c.session)) return true;

    if (c.running) {
        // Un flux lance par l'UI est deja en cours : on l'attend.
        while (c.running) {
            if (cancel && cancel->load(std::memory_order_relaxed))
                c.cancelReq.store(true);
            c.cv.wait_for(lk, 200ms);
        }
    } else {
        if (!begin_flow()) return false;
        lk.unlock();
        run_worker(/*force=*/false, cancel); // repose running=false + notifie
        lk.lock();
    }

    if (session_fresh(c.session)) return true;
    if (errOut)
        *errOut = c.error.empty() ? "Connexion Microsoft annulée." : c.error;
    return false;
}

void startup_refresh() {
    Ctx& c = ctx();
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (c.running) return;               // une connexion est deja en cours
        if (c.th.joinable()) c.th.join();
        if (DataStore::settings.accountMode != "microsoft") return; // hors ligne
        c.running = true;
        c.quiet = true; // aucun etat expose a l'UI pendant le renouvellement
        c.cancelReq.store(false);
    }
    c.th = std::thread([] {
        // (le detail de l'echec est journalise ci-dessous)
        try {
            // Sans jeton conserve, il n'y a rien a renouveler : l'utilisateur
            // se connectera quand il le decidera (pas de modale au demarrage).
            RenewFail why = RenewFail::None;
            auto s = silent_renew(Cancel{}, &why);
            (void)s;
            switch (why) {
            case RenewFail::RefreshRefused:
                auth_log("Démarrage : session Microsoft expirée, reconnexion "
                         "nécessaire au prochain lancement du jeu.");
                break;
            case RenewFail::Transient:
                auth_log("Démarrage : services Minecraft indisponibles, session "
                         "conservée (nouvelle tentative au prochain démarrage).");
                break;
            case RenewFail::NoToken:
            case RenewFail::None:
                break;
            }
        } catch (const Cancelled&) {
            // shutdown pendant la requete : rien a signaler
        } catch (const std::exception& ex) {
            // Hors ligne, DNS indisponible... : on garde le cache existant.
            auth_log(std::string("Démarrage : renouvellement impossible pour "
                                 "l'instant (") +
                     ex.what() + ")");
        }
        std::lock_guard<std::mutex> lk(ctx().m);
        ctx().running = false;
        ctx().quiet = false;
        // Etat volontairement laisse a Idle : ce rafraichissement est
        // silencieux, il ne doit jamais ouvrir la modale de connexion.
        ctx().state = AuthState::Idle;
        ctx().cv.notify_all();
    });
}

void cancel_login() { ctx().cancelReq.store(true); }

void logout() {
    cancel_login();
    std::error_code ec;
    fs::remove(token_file(), ec);
    // Divergence assumee vs C# (qui ne supprimait que msauth.json) : sans cela
    // le cache de session reconnecte tout seul pendant 7 jours.
    fs::remove(session_file(), ec);
    std::lock_guard<std::mutex> lk(ctx().m);
    if (ctx().session) secure_wipe(ctx().session->token); // pas de résidu
    ctx().session.reset();
    ctx().diskChecked = true;
    ctx().state = AuthState::Idle;
    ctx().info = LoginInfo{};
    ctx().error.clear();
    auth_log("Déconnexion : jeton et cache de session supprimés.");
}

bool has_session() { return get_session().has_value(); }

std::optional<AuthSession> get_session() {
    Ctx& c = ctx();
    {
        std::lock_guard<std::mutex> lk(c.m);
        if (session_usable(c.session)) return c.session;
        if (c.diskChecked || c.running) return std::nullopt;
        c.diskChecked = true; // une seule lecture disque (appel par frame UI)
    }
    auto disk = try_load_session_cache();
    if (!session_usable(disk)) return std::nullopt;
    std::lock_guard<std::mutex> lk(c.m);
    c.session = disk;
    return disk;
}

AuthState get_state() {
    std::lock_guard<std::mutex> lk(ctx().m);
    return ctx().state;
}

std::optional<LoginInfo> get_login_info() {
    std::lock_guard<std::mutex> lk(ctx().m);
    if (ctx().state == AuthState::Idle) return std::nullopt;
    return ctx().info;
}

void dismiss() {
    std::lock_guard<std::mutex> lk(ctx().m);
    if (ctx().state == AuthState::Done || ctx().state == AuthState::Error) {
        ctx().state = AuthState::Idle;
        ctx().info = LoginInfo{};
    }
}

void stop() {
    ctx().cancelReq.store(true);
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(ctx().m);
        th = std::move(ctx().th);
    }
    if (th.joinable()) th.join();
}

} // namespace tl::auth

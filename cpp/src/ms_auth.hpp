#pragma once

// Portage de MsAuth.cs — authentification Microsoft officielle
// (OAuth device code -> Xbox Live -> XSTS -> Minecraft).
//
// Fichiers (compatibles v5 C#, memes chemins, memes formats) :
//   <data>/msauth.json        jeton de rafraichissement, chiffre DPAPI + base64
//   <data>/session-cache.json session Minecraft (nom/uuid/token DPAPI + ts)
//
// Le flux reseau vit dans tl_core (ms_auth.cpp) ; seul draw_login_modal()
// est implemente cote UI (ui_auth.cpp) car il depend d'ImGui.

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "secrets.hpp" // base64 + DPAPI (module partage DataStore / ms_auth)

namespace tl::auth {

enum class AuthState {
    Idle,           // pas de tentative en cours
    RequestingCode, // appel initial vers Microsoft pour obtenir le device code
    WaitingCode,    // on attend que l'utilisateur valide dans le navigateur
    Chaining,       // on a le code, on echange les jetons (XBL -> XSTS -> MC)
    Done,           // succes, session active
    Error           // echec critique
};

struct AuthSession {
    std::string name;
    std::string uuid;
    std::string token;
    long long expiresAt = 0; // epoch s. du cache disque (ts + 604800)
};

// --- API publique ---

// ID client du launcher Minecraft officiel (aucune inscription Azure requise).
extern const char* const kDefaultClientId;

// Lance la connexion en arriere-plan (retour immediat). force=true ignore les
// caches memoire/disque et refait la chaine complete.
void login_start(bool force = false);

// Bloque jusqu'au succes ou a l'echec. Reutilise un flux deja lance par
// login_start(). Retourne true si une session Microsoft est disponible.
bool login_blocking(std::atomic<bool>* cancel = nullptr, std::string* errOut = nullptr);

// Renouvellement silencieux au demarrage du launcher (thread de fond).
//
// Fait glisser la fenetre de validite cote Microsoft tant que l'utilisateur se
// sert du launcher, et regenere le jeton Minecraft (~24 h) d'avance pour que
// « Jouer » parte sans attente. N'ouvre JAMAIS la modale : sans jeton conserve,
// ou si Microsoft refuse, l'appel ne fait rien de visible et la connexion sera
// redemandee au moment ou l'utilisateur lancera une partie.
void startup_refresh();

// Annule le flux en cours (bouton « Annuler » de la modale).
void cancel_login();

// Supprime le jeton de rafraichissement + le cache de session.
void logout();

// Une session valide existe (cache memoire ou disque non expire).
bool has_session();

// Oublie la session en memoire et force une relecture du disque au
// prochain appel. Necessaire apres une bascule de compte : les fichiers
// ont change sous nos pieds.
void reload_session();

// Session courante (nullopt si aucune).
std::optional<AuthSession> get_session();

// --- Hooks UI ---

AuthState get_state();

struct LoginInfo {
    std::string userCode;
    std::string verificationUrl;
    std::string message; // etat lisible, ou message d'erreur si state==Error
};
std::optional<LoginInfo> get_login_info();

// Ferme la modale apres un succes/echec lu par l'UI.
void dismiss();

// Dessine la modale de connexion (ui_auth.cpp) — boucle de rendu principale.
void draw_login_modal();

// Arrete le thread de fond (shutdown de l'app).
void stop();

// --- Exposes pour les tests (tests/test_ms_auth.cpp) ---
namespace detail {

// base64 + DPAPI vivent dans tl::secrets (module partage avec DataStore) :
// re-exportes ici pour ne pas casser les appels existants.
using tl::secrets::b64_encode;
using tl::secrets::b64_decode;
using tl::secrets::dpapi_protect_b64;
using tl::secrets::dpapi_unprotect_b64;

std::string url_encode(const std::string& s);
using Form = std::vector<std::pair<std::string, std::string>>;
std::string form_body(const Form& f);

// 32 hexa -> 8-4-4-4-12 (FormatUuid C#) ; retourne l'entree si longueur != 32.
std::string format_uuid(const std::string& hex);

// Effacement sûr d'un secret en mémoire (SecureZeroMemory, anti-optimiseur).
using tl::secrets::secure_wipe;

// Ecriture atomique (tmp + rename, cf. C# File.Replace) : pas de fichier
// tronqué si le process meurt ou si l'antivirus verrouille la destination.
bool write_atomic(const std::filesystem::path& dst, const std::string& data);

// Pseudo -> nom de fichier sûr ([A-Za-z0-9_-], 64 car. max, "skin" sinon).
std::string sanitize_file_stem(const std::string& s);

// Garde-fou avant ShellExecute : https + hôte Microsoft/Xbox connu. L'URL de
// validation vient du JSON serveur : même en TLS, on ne l'ouvre jamais aveugle.
bool browser_url_allowed(const std::string& url);

// <data>/session-cache.json et <data>/msauth.json
void save_session_cache(const AuthSession& s);
std::optional<AuthSession> try_load_session_cache();
void save_refresh_token(const std::string& token);
std::optional<std::string> try_load_refresh_token();

} // namespace detail

} // namespace tl::auth

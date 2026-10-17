#pragma once

// Page Musique — intégration SB'Music (https://dev.sb-music.com/docs).
//
// API REST v1 sur https://sb-music.com/api/v1, authentifiée par OAuth 2.0
// Authorization Code + PKCE (S256). Le launcher est un client PUBLIC : pas de
// client_secret (il serait extractible d'un binaire distribué, cf. M7) ; la
// preuve de possession passe par PKCE. Les jetons sont chiffrés au repos
// (DPAPI / trousseau) comme les autres secrets.
//
// Ce fichier couvre l'AUTHENTIFICATION. La recherche, les playlists et le
// streaming (phases suivantes) viendront dans sbmusic_api.*.

#include <string>

namespace tl::sbmusic {

// client_id OAuth de l'application « Team Launcher » (valeur PUBLIQUE).
const char* client_id();

// Un couple de jetons est-il mémorisé (connexion active, éventuellement à
// rafraîchir) ?
bool is_logged_in();

// Lance le flux OAuth : ouvre le navigateur système sur l'écran
// d'autorisation SB'Music, attend la redirection sur la boucle locale, échange
// le code contre des jetons et les mémorise. BLOQUANT (à appeler depuis un fil
// de travail, pas le fil UI). Rend true si la connexion a abouti ; `err`, s'il
// est fourni, reçoit un message en cas d'échec.
bool login(std::string* err = nullptr);

// Oublie les jetons mémorisés.
void logout();

// Jeton d'accès valide, rafraîchi si nécessaire. "" si non connecté ou si le
// rafraîchissement a échoué (il faut alors relancer login()).
std::string access_token();

} // namespace tl::sbmusic

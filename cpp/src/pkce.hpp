#pragma once

// PKCE (RFC 7636) pour OAuth 2.0 Authorization Code — flux des clients PUBLICS
// (apps natives sans client_secret). Le launcher s'en sert pour la page
// Musique (SB'Music) : on genere un code_verifier aleatoire, on envoie son
// code_challenge a l'ecran d'autorisation, puis on prouve la possession du
// verifier a l'echange du token. Un secret intercepte ne suffit alors pas.

#include <cstddef>
#include <string>

namespace tl::pkce {

// base64url SANS remplissage (RFC 4648 §5 : '+' -> '-', '/' -> '_', pas de
// '='), d'octets bruts. C'est l'encodage impose par PKCE.
std::string b64url(const unsigned char* data, std::size_t n);

// code_verifier : base64url de 32 octets tires d'un CSPRNG -> 43 caracteres,
// dans le jeu « unreserved » exige par RFC 7636 (43 a 128 caracteres).
std::string make_verifier();

// code_challenge = base64url(SHA256(verifier)) : la methode « S256 ».
std::string challenge(const std::string& verifier);

} // namespace tl::pkce

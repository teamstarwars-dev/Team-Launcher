#pragma once

// Secrets de config.json chiffres DPAPI (CurrentUser) + base64 — module
// partage DataStore / ms_auth.
//
//   - encode/decode base64  : identiques Convert.ToBase64String / FromBase64String
//   - protect/unprotect     : CryptProtectData sans entropie, UI interdite,
//                             meme format que ProtectedData.Protect (C#).
//   - encrypt_value         : marqueur "enc:v1:" + blob base64 pour les champs
//                             de config.json ; decrypt_value detecte le marqueur
//                             et laisse passer les valeurs en clair (compat
//                             anciens fichiers v5, ecrits par l'app Avalonia).

#include <cstddef>
#include <optional>
#include <string>

namespace tl::secrets {

// base64 « standard » (CRYPT_STRING_BASE64 | NOCRLF, sans \r\n).
std::string b64_encode(const unsigned char* data, std::size_t n);
std::optional<std::string> b64_decode(const std::string& s);

// DPAPI CurrentUser (CRYPTPROTECT_UI_FORBIDDEN, sans entropie).
std::optional<std::string> dpapi_protect_b64(const std::string& clear);
std::optional<std::string> dpapi_unprotect_b64(const std::string& b64);

// Effacement sûr d'un secret en mémoire (SecureZeroMemory, anti-optimiseur).
void secure_wipe(std::string& s);

// --- Champs de config.json ---
inline constexpr const char* kEncPrefix = "enc:v1:";

// "" -> "" (aucune protection d'une valeur vide : rien a cacher).
bool is_encrypted(const std::string& v);

// Clair -> "enc:v1:<base64 DPAPI>". Si DPAPI echoue, renvoie le clair tel quel
// (jamais de perte de donnee : le champ reste utilisable, juste non protege).
std::string encrypt_value(const std::string& clear);

// "enc:v1:..." -> clair ; valeur sans marqueur -> recopiee telle quelle
// (ancien format en clair). Un blob illisible donne le stockage brut (le champ
// reste present, il sera rechiffre au prochain enregistrement).
std::string decrypt_value(const std::string& stored);

} // namespace tl::secrets

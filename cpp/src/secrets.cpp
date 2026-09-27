#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wincrypt.h>

#include "secrets.hpp"

#include <string>

namespace tl::secrets {

// ---------------------------------------------------------------------------
// base64 (formats interoperables avec Convert.ToBase64String du C#)
// ---------------------------------------------------------------------------

std::string b64_encode(const unsigned char* data, std::size_t n) {
    DWORD chars = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(n), flags, nullptr, &chars))
        return {};
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA(data, static_cast<DWORD>(n), flags, out.data(), &chars))
        return {};
    out.resize(chars);
    return out;
}

std::optional<std::string> b64_decode(const std::string& s) {
    DWORD n = 0;
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                              CRYPT_STRING_BASE64, nullptr, &n, nullptr, nullptr))
        return std::nullopt;
    std::string out(n, '\0');
    if (!CryptStringToBinaryA(s.c_str(), static_cast<DWORD>(s.size()),
                              CRYPT_STRING_BASE64,
                              reinterpret_cast<BYTE*>(out.data()), &n, nullptr,
                              nullptr))
        return std::nullopt;
    out.resize(n);
    return out;
}

// DataProtectionScope.CurrentUser == CryptProtectData sans entropie.
std::optional<std::string> dpapi_protect_b64(const std::string& clear) {
    DATA_BLOB in{static_cast<DWORD>(clear.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(clear.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::string b64 = b64_encode(out.pbData, out.cbData);
    LocalFree(out.pbData);
    if (b64.empty()) return std::nullopt;
    return b64;
}

std::optional<std::string> dpapi_unprotect_b64(const std::string& b64) {
    auto raw = b64_decode(b64);
    if (!raw || raw->empty()) return std::nullopt;
    DATA_BLOB in{static_cast<DWORD>(raw->size()),
                 reinterpret_cast<BYTE*>(raw->data())};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return std::nullopt;
    std::string clear(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return clear;
}

// Les std::string ne s'effacent pas a la destruction : tout secret dont la
// duree de vie est terminee est ecrase explicitement. SecureZeroMemory est
// opaque pour l'optimiseur (pas d'elision).
void secure_wipe(std::string& s) {
    if (!s.empty()) ::SecureZeroMemory(s.data(), s.size());
}

// ---------------------------------------------------------------------------
// Champs de config.json
// ---------------------------------------------------------------------------

bool is_encrypted(const std::string& v) {
    return v.compare(0, std::char_traits<char>::length(kEncPrefix), kEncPrefix) == 0;
}

std::string encrypt_value(const std::string& clear) {
    if (clear.empty()) return clear;
    auto b64 = dpapi_protect_b64(clear);
    if (!b64) return clear; // DPAPI indisponible : on n'ecrase jamais la valeur
    return std::string(kEncPrefix) + *b64;
}

std::string decrypt_value(const std::string& stored) {
    if (stored.empty() || !is_encrypted(stored)) return stored; // ancien format clair
    auto clear = dpapi_unprotect_b64(stored.substr(std::char_traits<char>::length(kEncPrefix)));
    if (!clear) return stored; // blob illisible : on garde le stockage brut
    return *clear;
}

} // namespace tl::secrets

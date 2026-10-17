#include "pkce.hpp"

#include "secrets.hpp"   // b64_encode (base64 standard, partage avec DPAPI)
#include "util_hash.hpp" // sha256_digest (digest brut pour S256)

#include <random>

namespace tl::pkce {

std::string b64url(const unsigned char* data, std::size_t n) {
    std::string s = secrets::b64_encode(data, n); // base64 standard (+ / =)
    for (char& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string make_verifier() {
    // std::random_device est adosse au CSPRNG de l'OS (BCrypt/MSVC,
    // /dev/urandom sous libstdc++) : suffisant pour un secret ephemere PKCE.
    std::random_device rd;
    unsigned char buf[32];
    for (unsigned char& b : buf) b = static_cast<unsigned char>(rd() & 0xFFu);
    // 32 octets -> 43 caracteres base64url (sous-ensemble du jeu « unreserved »).
    return b64url(buf, sizeof(buf));
}

std::string challenge(const std::string& verifier) {
    const auto d = tl::sha256_digest(verifier);
    return b64url(d.data(), d.size());
}

} // namespace tl::pkce

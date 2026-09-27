// SHA-1 (RFC 3174) et MD5 (RFC 1321) — implementations portables.
//
// Etape 5 : ces deux fonctions passaient par BCrypt (Windows). Pour une cible
// Linux il fallait soit une deuxieme implementation, soit OpenSSL en
// dependance. Les deux algorithmes tiennent en ~120 lignes, sont figes depuis
// trente ans et sont couverts par les vecteurs de test officiels : on garde
// UNE seule implementation pour les deux plateformes, et le launcher perd une
// dependance systeme.
//
// Usage : verification d'integrite des telechargements Mojang (SHA-1 impose
// par les manifestes) et empreintes Modrinth. Ce ne sont pas des usages
// cryptographiques : la faiblesse connue de SHA-1 et MD5 face aux collisions
// n'entre pas en jeu, le protocole les impose.

#include "util_hash.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace tl {

namespace {

// --- SHA-1 -----------------------------------------------------------------

struct Sha1 {
    std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u,
                          0xC3D2E1F0u};
    std::uint8_t buf[64] = {};
    std::size_t len = 0;          // octets dans buf
    std::uint64_t total = 0;      // octets traites au total

    static std::uint32_t rol(std::uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    }

    void block(const std::uint8_t* p) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = (static_cast<std::uint32_t>(p[i * 4]) << 24) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 8) |
                   static_cast<std::uint32_t>(p[i * 4 + 3]);
        for (int i = 16; i < 80; ++i)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999u;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1u;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCu;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6u;
            }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    void update(const std::uint8_t* p, std::size_t n) {
        total += n;
        while (n > 0) {
            const std::size_t take = (std::min)(n, std::size_t{64} - len);
            std::memcpy(buf + len, p, take);
            len += take;
            p += take;
            n -= take;
            if (len == 64) {
                block(buf);
                len = 0;
            }
        }
    }

    void finish(std::uint8_t out[20]) {
        const std::uint64_t bits = total * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0x00;
        while (len != 56) update(&pad, 1);
        std::uint8_t tail[8];
        for (int i = 0; i < 8; ++i)
            tail[i] = static_cast<std::uint8_t>(bits >> (56 - i * 8));
        // `total` est fausse apres le bourrage, mais elle n'est plus lue.
        update(tail, 8);
        for (int i = 0; i < 5; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (24 - j * 8));
    }
};

// --- MD5 --------------------------------------------------------------------

struct Md5 {
    std::uint32_t h[4] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u};
    std::uint8_t buf[64] = {};
    std::size_t len = 0;
    std::uint64_t total = 0;

    static std::uint32_t rol(std::uint32_t v, int n) {
        return (v << n) | (v >> (32 - n));
    }

    void block(const std::uint8_t* p) {
        static const std::uint32_t K[64] = {
            0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu,
            0x4787c62au, 0xa8304613u, 0xfd469501u, 0x698098d8u, 0x8b44f7afu,
            0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u, 0xa679438eu,
            0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau,
            0xd62f105du, 0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u,
            0xc33707d6u, 0xf4d50d87u, 0x455a14edu, 0xa9e3e905u, 0xfcefa3f8u,
            0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
            0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u,
            0x289b7ec6u, 0xeaa127fau, 0xd4ef3085u, 0x04881d05u, 0xd9d4d039u,
            0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u, 0x432aff97u,
            0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du,
            0x85845dd1u, 0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u,
            0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu, 0xeb86d391u};
        static const int S[64] = {
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

        std::uint32_t m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = static_cast<std::uint32_t>(p[i * 4]) |
                   (static_cast<std::uint32_t>(p[i * 4 + 1]) << 8) |
                   (static_cast<std::uint32_t>(p[i * 4 + 2]) << 16) |
                   (static_cast<std::uint32_t>(p[i * 4 + 3]) << 24);

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            std::uint32_t f;
            int g;
            if (i < 16) {
                f = (b & c) | (~b & d);
                g = i;
            } else if (i < 32) {
                f = (d & b) | (~d & c);
                g = (5 * i + 1) % 16;
            } else if (i < 48) {
                f = b ^ c ^ d;
                g = (3 * i + 5) % 16;
            } else {
                f = c ^ (b | ~d);
                g = (7 * i) % 16;
            }
            const std::uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + K[i] + m[g], S[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }

    void update(const std::uint8_t* p, std::size_t n) {
        total += n;
        while (n > 0) {
            const std::size_t take = (std::min)(n, std::size_t{64} - len);
            std::memcpy(buf + len, p, take);
            len += take;
            p += take;
            n -= take;
            if (len == 64) {
                block(buf);
                len = 0;
            }
        }
    }

    void finish(std::uint8_t out[16]) {
        const std::uint64_t bits = total * 8;
        std::uint8_t pad = 0x80;
        update(&pad, 1);
        pad = 0x00;
        while (len != 56) update(&pad, 1);
        std::uint8_t tail[8];
        for (int i = 0; i < 8; ++i)
            tail[i] = static_cast<std::uint8_t>(bits >> (i * 8)); // petit-boutiste
        update(tail, 8);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                out[i * 4 + j] = static_cast<std::uint8_t>(h[i] >> (j * 8));
    }
};

const char* kHex = "0123456789abcdef";

} // namespace

std::optional<std::string> sha1_hex(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;

    Sha1 s;
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0)
            s.update(reinterpret_cast<const std::uint8_t*>(buf.data()),
                     static_cast<std::size_t>(got));
    }
    // `eof` est normal ; toute autre erreur invalide le resultat.
    if (in.bad()) return std::nullopt;

    std::uint8_t digest[20];
    s.finish(digest);
    std::string out;
    out.reserve(40);
    for (unsigned char c : digest) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0x0F]);
    }
    return out;
}

std::optional<std::array<unsigned char, 16>> md5_digest(std::string_view data) {
    Md5 m;
    m.update(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
    std::array<unsigned char, 16> out{};
    m.finish(out.data());
    return out;
}

} // namespace tl

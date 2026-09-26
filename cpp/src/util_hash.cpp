#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <bcrypt.h>

#include "util_hash.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace tl {

namespace {
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS 0x00000000L
#endif
struct AlgHandle {
    BCRYPT_ALG_HANDLE h = nullptr;
    ~AlgHandle() { if (h) BCryptCloseAlgorithmProvider(h, 0); }
};
struct HashHandle {
    BCRYPT_HASH_HANDLE h = nullptr;
    ~HashHandle() { if (h) BCryptDestroyHash(h); }
};
} // namespace

std::optional<std::string> sha1_hex(const fs::path& file) {
    AlgHandle alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_SHA1_ALGORITHM, nullptr, 0)
        != STATUS_SUCCESS)
        return std::nullopt;

    DWORD objLen = 0, cb = 0;
    if (BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0)
        != STATUS_SUCCESS)
        return std::nullopt;
    DWORD hashLen = 0;
    if (BCryptGetProperty(alg.h, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashLen), sizeof(hashLen), &cb, 0)
        != STATUS_SUCCESS || hashLen != 20)
        return std::nullopt;

    std::vector<UCHAR> obj(objLen);
    HashHandle hash;
    if (BCryptCreateHash(alg.h, &hash.h, obj.data(), objLen, nullptr, 0, 0)
        != STATUS_SUCCESS)
        return std::nullopt;

    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    std::vector<char> buf(1 << 16);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0 &&
            BCryptHashData(hash.h, reinterpret_cast<PUCHAR>(buf.data()),
                           static_cast<ULONG>(got), 0) != STATUS_SUCCESS)
            return std::nullopt;
    }

    std::vector<UCHAR> digest(hashLen);
    if (BCryptFinishHash(hash.h, digest.data(), hashLen, 0) != STATUS_SUCCESS)
        return std::nullopt;

    std::string out(hashLen * 2, '\0');
    for (DWORD i = 0; i < hashLen; ++i)
        std::snprintf(out.data() + i * 2, 3, "%02x", digest[i]);
    return out;
}

std::optional<std::array<unsigned char, 16>> md5_digest(std::string_view data) {
    AlgHandle alg;
    if (BCryptOpenAlgorithmProvider(&alg.h, BCRYPT_MD5_ALGORITHM, nullptr, 0)
        != STATUS_SUCCESS)
        return std::nullopt;
    DWORD objLen = 0, cb = 0;
    if (BCryptGetProperty(alg.h, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objLen), sizeof(objLen), &cb, 0)
        != STATUS_SUCCESS)
        return std::nullopt;
    std::vector<UCHAR> obj(objLen);
    HashHandle hash;
    if (BCryptCreateHash(alg.h, &hash.h, obj.data(), objLen, nullptr, 0, 0)
        != STATUS_SUCCESS)
        return std::nullopt;
    if (!data.empty() &&
        BCryptHashData(hash.h, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
                       static_cast<ULONG>(data.size()), 0) != STATUS_SUCCESS)
        return std::nullopt;
    std::array<unsigned char, 16> out{};
    if (BCryptFinishHash(hash.h, out.data(), static_cast<ULONG>(out.size()), 0)
        != STATUS_SUCCESS)
        return std::nullopt;
    return out;
}

} // namespace tl

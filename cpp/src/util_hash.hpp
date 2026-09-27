#pragma once

// SHA-1 et MD5 : implementations portables (RFC 3174 / RFC 1321).
// Verifications d integrite des telechargements Mojang et empreintes
// Modrinth. Plus aucune dependance a BCrypt : une seule implementation pour
// Windows et Linux.

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace tl {

// Hexa minuscule ; nullopt si lecture/hash impossible.
std::optional<std::string> sha1_hex(const std::filesystem::path& file);

// MD5 brut (16 octets) d'une donnee en memoire ; nullopt si echec.
std::optional<std::array<unsigned char, 16>> md5_digest(std::string_view data);

} // namespace tl

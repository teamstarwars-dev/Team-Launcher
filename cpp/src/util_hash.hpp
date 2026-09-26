#pragma once

// SHA1/MD5 via BCrypt (systeme) — verifications de telechargement Mojang.

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

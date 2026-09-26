#pragma once

// Wrappers miniz : extraction natives (.dll) + entree unique (install_profile.json)

#include <filesystem>
#include <optional>
#include <string>

namespace tl {

// Extrait tous les .dll d'un jar vers destDir (chemins d'entrees, garde-fou "..").
// Retourne le nombre d'entrees extraites (-1 = archive illisible).
int zip_extract_dlls(const std::filesystem::path& jar,
                     const std::filesystem::path& destDir);

// Extrait une entree nommee vers destPath. false = absente/echec.
bool zip_extract_entry(const std::filesystem::path& zip,
                       const std::string& entryName,
                       const std::filesystem::path& destPath);

// Lit une entree en memoire (manifest.json d'un modpack...). nullopt = absente.
std::optional<std::string> zip_read_entry(const std::filesystem::path& zip,
                                          const std::string& entryName);

// Extrait les entrees commencant par `prefix` vers destDir, prefixe retire
// ("overrides/" des modpacks). Garde-fou "..". -1 = archive illisible.
int zip_extract_prefix(const std::filesystem::path& zip, const std::string& prefix,
                       const std::filesystem::path& destDir);

// Extrait TOUTES les entrees (JRE Adoptium...) vers destDir.
// Garde-fou ".." (zip-slip). -1 = archive illisible, sinon nb d'entrees ecrites.
int zip_extract_all(const std::filesystem::path& zip,
                    const std::filesystem::path& destDir);

// Compresse recursivement dir vers zipPath (export d'instance, C# ZipFile).
// false = echec ou dossier vide.
bool zip_create_from_dir(const std::filesystem::path& dir,
                         const std::filesystem::path& zipPath);

} // namespace tl

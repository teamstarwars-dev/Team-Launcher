#pragma once

// Portage de SkinService.cs / SkinTools.cs : application d'un skin hors-ligne
// via le mod CustomSkinLoader + telechargement du skin officiel (mc-heads).

#include <atomic>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace tl::skin {

// Applique skinPath a une instance : installe CustomSkinLoader (Modrinth,
// loader Forge) si absent, puis copie le skin en
// config/CustomSkinLoader/LocalSkin/<pseudo>.png.
// Retourne le nom du fichier .jar pose ; lance std::runtime_error (FR).
std::string apply(const nlohmann::json& inst, const std::string& skinPath,
                  const std::string& playerName,
                  const std::atomic<bool>* cancel = nullptr);

// https://mc-heads.net/skin/<name> -> dest. false si echec.
bool download_by_name(const std::string& name,
                      const std::filesystem::path& dest,
                      const std::atomic<bool>* cancel = nullptr);

// Derniere version « release » officielle (manifeste Mojang, cachee).
std::string latest_release();

} // namespace tl::skin

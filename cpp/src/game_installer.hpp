#pragma once

// Portage de GameInstaller.cs — telechargement des fichiers officiels Mojang
// (version json, client.jar, bibliotheques, natives, assets) + Fabric/Forge/NeoForge.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl {

// TL_RUNTIME_DIR (test) > %LOCALAPPDATA%\TeamLauncher\runtime (comme le C#)
std::filesystem::path runtime_root();

struct CancelledError : std::runtime_error {
    CancelledError() : std::runtime_error("annule") {}
};

// progress(stade, fait, total)
using InstallProgress = std::function<void(const char*, int, int)>;

// Installe/verifie tout puis retourne le JSON de lancement (camelCase, meme
// contrat que le C#) : mainClass, classpath, jar, natives, assetsIndex,
// javaMajor, minecraftArguments, hasArguments, jvmArgs, [isForge].
nlohmann::json install(const std::string& versionId, const std::string& loader,
                       InstallProgress progress, std::atomic<bool>& cancel,
                       bool forceVerify = false);

// Exposes pour tests + launcher.
std::string maven_name_to_path(const std::string& name); // "" si rejetee
bool rules_allow(const nlohmann::json& lib);
std::vector<std::string> extract_jvm_args(const nlohmann::json& root);

// Installeurs de loaders (retournent l'id de version cree).
std::string ensure_fabric_installed(const std::string& mcVersion,
                                    std::atomic<bool>& cancel);
std::string ensure_forge_installed(const std::string& mcVersion,
                                   std::atomic<bool>& cancel);
std::string ensure_neoforge_installed(const std::string& mcVersion,
                                      std::atomic<bool>& cancel);

} // namespace tl

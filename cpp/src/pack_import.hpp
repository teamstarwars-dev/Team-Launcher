#pragma once

// Portage de CfPackImporter.cs et MrPackImporter.cs — import d'un modpack
// CurseForge (.zip, manifest.json) ou Modrinth (.mrpack, modrinth.index.json)
// vers une nouvelle instance.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace tl::packs {

// progress("Téléchargement des mods (3/42)…")
using Progress = std::function<void(const std::string&)>;

struct Result {
    nlohmann::json instance; // objet instance a ajouter au config (null si echec)
    int downloaded = 0;
    int failed = 0;
    bool cancelled = false;
    std::string error; // vide = succes
};

// Type detecte d'apres le contenu de l'archive (et non l'extension).
enum class Kind { Unknown, CurseForge, Modrinth };
Kind detect(const std::filesystem::path& archive);

// Importe le modpack. N'ecrit PAS dans le config : l'appelant ajoute
// `result.instance` et appelle DataStore::save() (le C# le faisait dedans).
Result import_curseforge(const std::filesystem::path& zip, const Progress& progress,
                         std::atomic<bool>& cancel);
Result import_modrinth(const std::filesystem::path& mrpack, const Progress& progress,
                       std::atomic<bool>& cancel);

// Aiguille selon detect(). Erreur claire si le format n'est pas reconnu.
Result import_any(const std::filesystem::path& archive, const Progress& progress,
                  std::atomic<bool>& cancel);

} // namespace tl::packs

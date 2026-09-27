#pragma once

// Installation d'un contenu (mod, shader, modpack) depuis Modrinth ou
// CurseForge vers une instance — portage de ExplorePage.InstallHitAsync et de
// ses aides. Separe de l'UI pour rester testable.

#include <atomic>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "curseforge.hpp"

namespace tl::content {

enum class Category { Modpacks, Mods, Shaders };

const char* category_key(Category c);            // "modpack"/"mod"/"shader" (Modrinth)
int curseforge_class(Category c);                // classId CurseForge
const char* dest_subdir(Category c);             // "mods" / "shaderpacks"

// Loader normalise pour les API : forge/fabric/neoforge tels quels, quilt ->
// fabric. nullopt si l'instance est en Vanilla (aucun mod installable).
std::optional<std::string> resolve_loader(const nlohmann::json& inst);

// McVersion de l'instance, ou la derniere release Mojang si "latest"/"?"/"".
// Chaine vide si le manifeste Mojang est injoignable.
std::string resolve_mc_version(const nlohmann::json& inst);

// Premier fichier CurseForge compatible. Regle fidele au C# : la version de
// jeu doit correspondre, et le loader doit correspondre OU le fichier ne
// declarer aucun loader. nullptr si rien ne convient.
const cf::File* pick_compatible(const std::vector<cf::File>& files,
                                const std::string& mcVersion,
                                const std::string& loader);

struct Outcome {
    bool ok = false;
    std::string error;
    std::string installed;             // chemin du fichier pose (mods/shaders)
    nlohmann::json instance = nullptr; // instance creee (modpack), sinon null
};

// slugOrId : slug Modrinth, ou identifiant numerique de projet CurseForge.
Outcome install(bool fromCurseForge, const std::string& slugOrId,
                const nlohmann::json& inst, Category cat,
                std::atomic<bool>& cancel);

} // namespace tl::content

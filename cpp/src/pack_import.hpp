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

// progress("Téléchargement des mods (3/42)...")
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

// --- Dossier d'instance existant ---------------------------------------
//
// Ce qu'un dossier d'instance venu d'un autre launcher dit de lui-meme.
//
// « Importer un dossier » creait l'instance en dur en Vanilla « latest »,
// sans regarder son contenu. Une instance CurseForge Forge 1.12.2 avec 39
// mods devenait donc une instance Vanilla de la derniere version : Minecraft
// ignorait le dossier mods/, et la verification d'avant-lancement le
// signalait — a juste titre, mais trop tard pour que l'utilisateur
// comprenne d'ou venait l'erreur.
struct FolderInfo {
    std::string name;              // vide = inconnu (l'appelant prend le dossier)
    std::string loader = "Vanilla"; // memes libelles que kInstanceLoaders
    std::string mcVersion = "latest";
    std::filesystem::path gameDir; // racine du jeu a copier (Prism : .minecraft/)
    std::string source;            // « CurseForge », « Prism », « Modrinth »,
                                   // « mods » (estimation), ou vide
};

// Lit, dans l'ordre : minecraftinstance.json (CurseForge), mmc-pack.json +
// instance.cfg (Prism / MultiMC), profile.json (Modrinth App). A defaut,
// estime chargeur et version d'apres les manifestes des mods. Ne lance
// jamais ; un dossier sans rien d'exploitable rend Vanilla « latest ».
FolderInfo detect_folder(const std::filesystem::path& dir);

// Importe le modpack. N'ecrit PAS dans le config : l'appelant ajoute
// `result.instance` et appelle DataStore::save() (le C# le faisait dedans).
Result import_curseforge(const std::filesystem::path& zip, const Progress& progress,
                         std::atomic<bool>& cancel);
Result import_modrinth(const std::filesystem::path& mrpack, const Progress& progress,
                       std::atomic<bool>& cancel);

// Aiguille selon detect(). Erreur claire si le format n'est pas reconnu.
Result import_any(const std::filesystem::path& archive, const Progress& progress,
                  std::atomic<bool>& cancel);

// Verifie le corps telecharge contre l'empreinte DECLAREE dans une entree
// .mrpack (objet « hashes » : sha512 de preference, sinon sha1). false si
// l'empreinte est absente (le format Modrinth l'impose) ou non conforme :
// un fichier non verifiable ne doit jamais atterrir dans l'instance.
// Expose pour les tests de regression (securite M2).
bool mrpack_body_verified(const nlohmann::json& fileEntry, const std::string& body);

} // namespace tl::packs

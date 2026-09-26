#pragma once

// Portage de CurseForgeApi.cs — client api.curseforge.com.
//
// La cle vient de `DataStore::settings.curseForgeApiKey`, avec repli sur la
// variable d'environnement CURSEFORGE_API_KEY (comme le C#). Elle n'est JAMAIS
// ecrite dans le depot : elle vit dans le config.json de l'utilisateur.

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace tl::cf {

// Classes de projets CurseForge
inline constexpr int kClassMods = 6;
inline constexpr int kClassModpacks = 4471;
inline constexpr int kClassResourcePacks = 12;
inline constexpr int kClassShaders = 6552;

struct Hit {
    int projectId = 0;
    std::string slug;
    std::string title;
    long long downloads = 0;
    std::string description;
    std::string loaders; // "forge fabric" (dedoublonne, ordre d'apparition)
    std::string iconUrl;
};

struct File {
    long long fileId = 0;
    int modId = 0;
    std::string fileName;
    std::string displayName;
    std::string downloadUrl; // vide = URL absente (CDN de secours)
    std::vector<std::string> gameVersions;
    std::vector<std::string> loaders;
    std::string fileDate; // ISO 8601 brut (tri lexicographique = chronologique)
};

// Cle configuree (reglages ou CURSEFORGE_API_KEY). Vide = non configuree.
std::string api_key();
bool has_key();

// Message d'aide identique au C# quand la cle manque.
const char* missing_key_message();

// Recherche (sortField=2 popularite, 25 resultats). Leve en cas d'echec.
std::vector<Hit> search(const std::string& query, int classId,
                        const std::atomic<bool>* cancel = nullptr);

// Fichiers d'un projet, du plus recent au plus ancien.
std::vector<File> get_files(int projectId, const std::atomic<bool>* cancel = nullptr);

// POST /mods/files par lots de 64.
std::vector<File> get_files_by_ids(const std::vector<long long>& fileIds,
                                   const std::atomic<bool>* cancel = nullptr);

// POST /mods par lots de 64 : projectId -> classId (defaut kClassMods).
std::map<int, int> get_project_classes(const std::vector<int>& projectIds,
                                       const std::atomic<bool>* cancel = nullptr);

// Telecharge vers destDir (nom assaini). Retourne le chemin, ou "" si echec.
std::string download_file(const File& f, const std::filesystem::path& destDir,
                          const std::atomic<bool>* cancel = nullptr);

// Empreinte MurmurHash2 (variante CurseForge : \t \n \r retires avant hachage).
long long compute_fingerprint(const std::filesystem::path& file);
long long murmur2(const std::string& data, unsigned seed = 0x1F123BB5u);

// Nom de fichier sans caractere interdit par Windows.
std::string sanitize(const std::string& name);

// modLoader numerique -> "forge"/"fabric"/"quilt"/"neoforge" ("" sinon).
const char* loader_name(int modLoader);

} // namespace tl::cf

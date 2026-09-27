#pragma once

// Portage de ModrinthApi (Apis.cs) — api.modrinth.com, sans cle d'API.

#include <atomic>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace tl::mr {

struct Hit {
    std::string title;
    std::string slug;
    std::string type; // "mod", "modpack", "shader"
    long long downloads = 0;
    std::string description;
    std::string loaders; // "forge fabric" (categories filtrees)
    std::string iconUrl;
};

// projectType : "mod", "modpack", "shader". Leve en cas d'echec.
std::vector<Hit> search(const std::string& query, const std::string& projectType,
                        const std::atomic<bool>* cancel = nullptr);

// Resolution par empreinte : POST /v2/version_files {hashes, algorithm:"sha1"}.
// Rend, pour chaque SHA1 reconnu, le projet et l'URL de telechargement.
struct FileMatch {
    std::string projectId;
    std::string url;
    std::string filename;
};
std::map<std::string, FileMatch> version_files(const std::vector<std::string>& sha1s,
                                               const std::atomic<bool>* cancel = nullptr);

// Telecharge le dernier fichier compatible d'un projet vers destDir.
// loader/mcVersion vides = aucun filtre. Retourne le chemin ecrit ; leve si
// aucune version compatible.
std::string download_project_file(const std::string& slug,
                                  const std::filesystem::path& destDir,
                                  const std::string& loader = {},
                                  const std::string& mcVersion = {},
                                  const std::atomic<bool>* cancel = nullptr);

} // namespace tl::mr

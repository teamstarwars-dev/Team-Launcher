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

// Meme chose, avec des facettes : chargeur et version de Minecraft (vides
// = aucun filtre) et un tri ("relevance", "downloads", "follows",
// "newest", "updated").
//
// C'est ce qui fait la difference entre un palmares et une recommandation :
// proposer le mod le plus telecharge du moment n'aide personne s'il ne
// tourne pas sur la version qu'on joue.
std::vector<Hit> search_filtered(const std::string& query,
                                 const std::string& projectType,
                                 const std::string& loader,
                                 const std::string& mcVersion,
                                 const std::string& sortIndex,
                                 const std::atomic<bool>* cancel = nullptr);

// Resolution par empreinte : POST /v2/version_files {hashes, algorithm:"sha1"}.
// Rend, pour chaque SHA1 reconnu, le projet et l'URL de telechargement.
struct FileMatch {
    std::string projectId;
    std::string url;
    std::string filename;
    std::string versionNumber; // version installee, telle que declaree
};
std::map<std::string, FileMatch> version_files(const std::vector<std::string>& sha1s,
                                               const std::atomic<bool>* cancel = nullptr);

// Une version publiee d'un projet. `changelog` est le texte redige par
// l'auteur : c'est lui qui permet de decider si une mise a jour vaut le
// risque, plutot que de la prendre a l'aveugle.
struct Version {
    std::string id;
    std::string projectId;
    std::string name;          // titre de la version
    std::string number;        // « version_number », ce qu'on compare
    std::string changelog;
    std::string url;           // fichier principal
    std::string filename;
    std::string datePublished; // ISO 8601
};

// Versions d'un projet, filtrees par chargeur et version de Minecraft
// (vides = aucun filtre), telles que Modrinth les rend : la plus recente
// d'abord. Liste vide = aucune version compatible, ou requete impossible.
// Ne leve pas : l'appelant tourne dans un travailleur et veut un constat.
std::vector<Version> project_versions(const std::string& projectIdOrSlug,
                                      const std::string& loader,
                                      const std::string& mcVersion,
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

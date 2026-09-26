#pragma once

// Portage de BackupService.cs — sauvegardes zip du dossier `saves` d'une
// instance, rotation sur les 10 plus recentes, restauration.

#include <filesystem>
#include <string>
#include <vector>

namespace tl::backup {

// <instances>/<id>/backups
std::filesystem::path dir(const std::string& instanceId);

// Zippe <instances>/<id>/saves vers backups/mondes-AAAA-MM-JJ_HH-mm.zip.
// Retourne le chemin cree, ou "" si le dossier saves est absent ou vide.
std::string create(const std::string& instanceId);

struct Entry {
    std::filesystem::path file;
    long long mtime = 0; // epoch s.
};

// Sauvegardes de l'instance, plus recente en premier.
std::vector<Entry> list(const std::string& instanceId);

// Remplace le contenu de saves par celui de l'archive. false = echec
// (le dossier saves existant n'est supprime qu'apres extraction reussie).
bool restore(const std::string& instanceId, const std::filesystem::path& zip);

bool remove(const std::filesystem::path& zip);

// Nombre maximum d'archives conservees (MaxBackups C#).
inline constexpr int kMaxBackups = 10;

} // namespace tl::backup

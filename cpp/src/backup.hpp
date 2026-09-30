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
    long long bytes = 0; // taille de l'archive : sans elle, impossible de
                         // choisir quoi supprimer quand la place manque
};

// Sauvegardes de l'instance, plus recente en premier.
std::vector<Entry> list(const std::string& instanceId);

// Remplace le contenu de saves par celui de l'archive. false = echec
// (le dossier saves existant n'est supprime qu'apres extraction reussie).
bool restore(const std::string& instanceId, const std::filesystem::path& zip);

bool remove(const std::filesystem::path& zip);

// Nombre maximum d'archives conservees (MaxBackups C#).
inline constexpr int kMaxBackups = 10;

// --- Rotation ---------------------------------------------------------------

// Supprime les archives en trop : au-dela de `keep`, puis au-dela de
// `maxBytes` au total (0 = pas de limite de place). Les plus ANCIENNES
// partent d'abord. Retourne le nombre supprime.
//
// Le quota de place compte autant que le nombre : dix sauvegardes d'un
// monde de 2 Go remplissent 20 Go sans que personne l'ait demande. Le
// reglage existait dans la configuration mais n'etait applique nulle part.
int rotate(const std::string& instanceId, int keep, long long maxBytes);

// --- Sauvegarde automatique -------------------------------------------------

// Une sauvegarde est-elle due ? Fonction PURE, donc testable sans attendre
// des heures. `everyHours <= 0` desactive. `lastUnix == 0` (jamais
// sauvegarde) rend la sauvegarde due immediatement.
bool is_due(long long nowUnix, long long lastUnix, int everyHours);

// Horodatage de la sauvegarde la plus recente, 0 s'il n'y en a aucune.
long long last_backup_unix(const std::string& instanceId);

// Demarre / arrete le minuteur de sauvegarde automatique. Sans effet si
// l'intervalle configure est nul. Le minuteur ne sauvegarde JAMAIS pendant
// qu'une partie tourne : zipper un monde en cours d'ecriture produirait une
// archive incoherente.
void auto_start();
void auto_stop();

// Le jeu tourne-t-il ? Renseigne par l'interface ; le minuteur s'abstient
// tant que c'est vrai.
void set_game_running(bool running);

} // namespace tl::backup

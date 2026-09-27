#pragma once

// Portage de WorldSyncService.cs — rapprochement des mondes entre les
// instances CurseForge du PC et celles du launcher, par nom d'instance.
//
// CurseForge range ses mondes dans <instance>/minecraft/saves, le launcher
// dans <instance>/saves : les deux emplacements sont examines.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::wsync {

enum class Origin { CurseForge, Launcher };

struct Snapshot {
    std::string instanceName;  // dossier ou nom d'instance
    std::string worldFolder;   // nom du dossier du monde
    std::string displayName;   // LevelName, sinon le dossier
    std::filesystem::path path;
    Origin origin = Origin::CurseForge;
    std::int64_t lastModified = 0;   // epoch s. du dossier (activite en jeu)
    std::int64_t levelLastPlayed = 0; // epoch ms depuis level.dat, 0 = inconnu
    std::int64_t sizeBytes = 0;
};

struct Compare {
    std::string launcherInstanceId; // vide = aucune instance de meme nom
    std::string instanceName;
    std::filesystem::path curseForgePath;
    std::vector<Snapshot> newer;          // plus recents cote CurseForge
    std::vector<Snapshot> onlyInLauncher;
    std::vector<Snapshot> onlyInCurseForge;
};

// <profil>/curseforge/minecraft/Instances/*
std::vector<std::pair<std::filesystem::path, std::string>>
detect_curseforge_instances();

// Mondes d'une racine d'instance (teste minecraft/saves puis saves).
std::vector<Snapshot> list_worlds_in(const std::filesystem::path& instanceRoot,
                                     Origin origin);

// Rapprochement de toutes les instances CurseForge avec celles du launcher.
std::vector<Compare> compare_all();

// Mondes plus recents cote CurseForge et absents du launcher.
std::vector<Snapshot> detect_newer_from_curseforge();

struct ImportResult {
    bool ok = false;
    std::string error;
    std::filesystem::path target;
    std::filesystem::path backup; // sauvegarde du monde ecrase, si besoin
};

// Copie un monde CurseForge dans une instance du launcher. Le monde existant
// est d'abord archive (zip) puis remplace.
ImportResult import_world(const Snapshot& w, const std::string& targetInstanceId);

} // namespace tl::wsync

#pragma once

// Mondes Minecraft d'une instance — portage de WorldTools.cs.
//
// CORRECTIF vs C# : `ReadLevelDat` cherchait la suite d'octets « LevelName »
// n'importe ou dans le NBT decompresse, puis lisait une longueur juste apres.
// C'etait un contournement de NbtReader (casse, cf. nbt.hpp) : la premiere
// occurrence pouvait tomber dans une autre donnee, et rien ne garantissait
// qu'on lisait le bon tag. Ici le fichier est reellement analyse.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tl::world {

struct Info {
    std::string folder;        // nom du dossier
    std::filesystem::path path;
    std::string name;          // LevelName
    std::int64_t lastPlayed = 0;   // epoch ms (Java), 0 = inconnu
    std::int64_t dataVersion = 0;  // 0 = inconnu (mondes anciens)
    std::int64_t seed = 0;
    int gameType = -1;         // 0 survie, 1 creatif, 2 aventure, 3 spectateur
    bool hardcore = false;
    std::int64_t sizeBytes = 0;
    int regionCount = 0;
};

// Lecture de <worldDir>/level.dat. nullopt si absent ou illisible.
std::optional<Info> read_level(const std::filesystem::path& worldDir);

// Mondes d'une instance (<instanceDir>/saves/*), tries du plus recemment
// joue au plus ancien. Les dossiers sans level.dat sont ignores.
std::vector<Info> list_worlds(const std::filesystem::path& instanceDir);

// Version de Minecraft deduite de DataVersion ("" si inconnue).
std::string version_name(std::int64_t dataVersion);

// Regions vides ou corrompues (<= 8 Ko, soit l'en-tete seul).
int count_empty_regions(const std::filesystem::path& worldDir);
int delete_empty_regions(const std::filesystem::path& worldDir);

// Taille totale d'un dossier, en octets (recursif, erreurs ignorees).
std::int64_t dir_size(const std::filesystem::path& p);

} // namespace tl::world

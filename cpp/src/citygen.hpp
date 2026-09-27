#pragma once

// Portage de CityGenerator.cs — generation d'une ville reelle a partir des
// donnees OpenStreetMap (API Overpass), projetee en coordonnees Minecraft.
//
// Malgre le nom « Arnis » employe dans l'UI du C#, ce service ne pilote aucun
// outil externe : il interroge Overpass, classe les entites (batiments,
// routes, eau, parcs, rails) et les rasterise en blocs.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tl::citygen {

struct BBox {
    double minLon = 0, minLat = 0, maxLon = 0, maxLat = 0;
    bool valid = false;
};

// "minLon,minLat,maxLon,maxLat". Le point decimal est exige : la virgule est
// deja le separateur de champs.
BBox parse_bbox(const std::string& s);

enum class Kind { Unknown, Building, Highway, Water, Park, Railway };

struct Point {
    int x = 0;
    int z = 0;
};

struct Entity {
    std::int64_t id = 0;
    Kind kind = Kind::Unknown;
    std::vector<Point> points; // deja en coordonnees Minecraft
    int height = 0;            // batiments : hauteur en blocs
    int width = 0;             // voies : largeur en blocs
};

struct OsmData {
    std::vector<Entity> entities;
    double centerLon = 0, centerLat = 0;
    int nodes = 0; // noeuds lus (diagnostic)
};

// Nom de bloc Minecraft associe a un type d'entite.
const char* block_for(Kind k);

// Analyse d'une reponse Overpass. Fonction pure : testable hors ligne.
OsmData parse_overpass(const std::string& json, const BBox& box);

using Progress = std::function<void(const std::string&)>;

// Interroge overpass-api.de. Leve en cas d'echec reseau ou de refus.
OsmData fetch_osm(const BBox& box, const Progress& progress,
                  const std::atomic<bool>* cancel = nullptr);

// --- rasterisation (pure) ---

struct Block {
    int x = 0, y = 0, z = 0;
    const char* name = nullptr;
};

// Un point est-il dans le polygone (lancer de rayon) ?
bool point_in_polygon(int px, int pz, const std::vector<Point>& poly);

// Remplit un polygone a la hauteur voulue (batiments, plans d'eau, parcs).
void fill_polygon(std::vector<Block>& out, const std::vector<Point>& poly,
                  int baseY, int height, const char* name);

// Trace une ligne epaisse (voies, rails).
void fill_line(std::vector<Block>& out, const std::vector<Point>& pts, int width,
               int y, const char* name);

// Rasterise toutes les entites. `maxBlocks` borne le resultat : une emprise
// trop large produirait des centaines de millions de blocs.
std::vector<Block> rasterize(const OsmData& data, int baseY,
                             std::size_t maxBlocks = 4000000);

// --- ecriture dans un monde ---

struct PasteResult {
    int placed = 0;        // blocs effectivement poses
    int skipped = 0;       // section absente (hors des couches generees)
    int unsupported = 0;   // nom de bloc inconnu (monde <= 1.12)
    int chunksWritten = 0;
    int chunksMissing = 0; // chunk jamais genere : rien n'est cree
    std::string error;     // vide si tout s'est bien passe
};

// Pose les blocs rasterises dans un monde existant. `originX`/`originZ` sont
// ajoutes aux coordonnees (le centre de l'emprise OSM tombe a l'origine).
//
// Le monde doit avoir ete genere au prealable : un chunk absent n'est PAS
// cree (il faudrait fabriquer terrain, biomes et lumiere), il est compte dans
// `chunksMissing`. Meme parti pris que le C#, mais celui-ci ecrivait un NBT
// petit-boutiste : rien n'etait jamais relisible par Minecraft.
//
// Faire une sauvegarde du monde avant d'appeler : chaque fichier de region est
// remplace atomiquement, mais pas l'ensemble.
PasteResult paste_into_world(const std::filesystem::path& worldDir,
                             const std::vector<Block>& blocks, int originX,
                             int originZ, const Progress& progress,
                             const std::atomic<bool>* cancel = nullptr);

} // namespace tl::citygen

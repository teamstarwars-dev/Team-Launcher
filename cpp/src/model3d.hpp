#pragma once

// Lecture de modeles 3D Minecraft — portage de ModelViewer3D.cs.
//
// Trois formats, tous a base de boites alignees sur les axes :
//   - `.bbmodel`  (Blockbench)      : `elements` avec `from`/`to`, ou `cubes`
//                                     avec `origin`/`size`
//   - `.json`     (modele Java)     : `elements` avec `from`/`to`
//   - `.geo.json` (geometrie Bedrock) : `minecraft:geometry[].bones[].cubes[]`
//
// CORRECTIFS vs C# :
//
//  1. **`ModelViewerPage.LoadModel_Click` n'ouvrait jamais le fichier choisi.**
//     Il retenait le chemin, puis appelait `cubes.Clear(); LoadDefaultModel();`
//     — le mannequin code en dur s'affichait quel que soit le fichier. Pire,
//     `ModelViewer3D`, la seule classe qui savait analyser un `.bbmodel`,
//     n'etait instanciee nulle part. Ici le fichier est reellement lu.
//  2. Le filtre du selecteur annoncait `.obj`, format qu'aucun code ne lisait.
//     Il n'est plus propose (une soupe de triangles ne rentre pas dans ce
//     modele a boites) — c'est dit dans l'interface.
//  3. `ParseBbmodel` cherchait une propriete litteralement nommee « cubes »
//     avec une espace en tete, vestige d'un contournement. Abandonne.
//  4. `origin` d'un `.bbmodel` est le PIVOT de rotation, pas la position. Le
//     C# le lisait dans X/Y/Z avant de l'ecraser avec `from`. Ici `origin`
//     n'est utilise qu'avec `size` (ancien format), jamais avec `from`/`to`.
//  5. La rotation d'element (`rotation`) reste ignoree, comme dans le C# :
//     c'est dit a l'ecran plutot que silencieux.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tl::model3d {

// Boite alignee sur les axes. (x, y, z) est le coin minimal.
struct Box {
    float x = 0, y = 0, z = 0;
    float w = 0, h = 0, d = 0;
    std::uint32_t color = 0xFF808080; // 0xAARRGGBB
    bool rotated = false;             // rotation presente et ignoree
};

struct Model {
    std::vector<Box> boxes;
    std::string name;
    std::string format;   // « bbmodel », « java », « bedrock », « defaut »
    int ignoredRotations = 0;
    bool empty() const { return boxes.empty(); }
};

struct Bounds3 {
    float minX = 0, minY = 0, minZ = 0;
    float maxX = 0, maxY = 0, maxZ = 0;
    float cx() const { return (minX + maxX) * 0.5f; }
    float cy() const { return (minY + maxY) * 0.5f; }
    float cz() const { return (minZ + maxZ) * 0.5f; }
    float size() const;  // plus grande dimension, 1 minimum
};

Bounds3 bounds_of(const Model& m);

// Analyse un contenu JSON. `ext` oriente le choix du format (« .bbmodel »,
// « .geo.json », « .json ») ; a defaut les trois sont essayes. Modele vide si
// rien n'est exploitable — jamais d'exception.
Model parse(const std::string& json, const std::string& ext);

// Lit un fichier. nullopt si illisible ; un modele vide si le format n'a rien
// donne (l'appelant distingue les deux).
std::optional<Model> load(const std::filesystem::path& p);

// Le mannequin code en dur du C# (LoadDefaultModel), conserve comme modele de
// demonstration au premier affichage.
Model mannequin();

} // namespace tl::model3d

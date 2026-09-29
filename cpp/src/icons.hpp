#pragma once

// Jeu d'icones de l'interface, dessine en vectoriel.
//
// Pourquoi ne PAS utiliser une police d'icones (Font Awesome, Lucide,
// Material Symbols), qui est l'usage courant avec Dear ImGui :
//
//  1. Il faudrait redistribuer un fichier de police avec le launcher, donc
//     en respecter la licence et l'attribution. Le projet ne redistribue
//     aucune police : celle de l'interface vient du systeme (voir fonts.hpp).
//  2. Un atlas d'icones pese quelques centaines de kilooctets pour la
//     vingtaine de symboles reellement utilises, sur un binaire qui vise
//     3 Mo.
//  3. Une police d'icones est rendue a une taille fixe puis mise a l'echelle,
//     donc floue des que l'echelle du texte change. Ici chaque icone est
//     retracee a la taille demandee, nette a tous les corps.
//
// Le cout : chaque icone est du code. Elles sont donc volontairement
// simples, au trait, dessinees sur une grille de 24x24 ramenee a la taille
// voulue — de quoi rester lisibles a 16 px comme a 48 px.

#include "imgui.h"

namespace tl::ui::icons {

enum class Id {
    Home,       // accueil
    Instances,  // grille de cartes
    Explore,    // boussole (decouverte de contenu)
    Files,      // dossier (explorateur)
    Map,        // carte pliee (editeur de cartes)
    City,       // immeubles (generateur de ville)
    ModDev,     // marteau (developpement de mods)
    Model,      // cube en perspective (modeles 3D)
    Play,       // triangle de lecture
    Servers,    // baies empilees
    Skins,      // buste
    News,       // journal
    Bedrock,    // bloc
    Account,    // personne
    Settings,   // engrenage
    Download,   // fleche vers un plateau
    Help,       // point d'interrogation cercle
    Search,     // loupe
    Check,      // coche
    Close,      // croix
    Pause,      // deux barres
    Trash,      // corbeille
    Star,       // etoile (favoris)
    Folder,     // dossier ouvert
};

// Dessine l'icone dans le carre (pos, pos+size). `col` est la couleur du
// trait. `thickness` <= 0 laisse choisir une epaisseur proportionnelle a la
// taille, ce qui garde le meme poids visuel a toutes les echelles.
void draw(ImDrawList* dl, Id id, ImVec2 pos, float size, ImU32 col,
          float thickness = 0.0f);

// Bouton de navigation « icone + libelle », pour la barre laterale.
// `compact` n'affiche que l'icone et pose une infobulle avec le libelle.
bool nav_item(Id id, const char* label, bool active, bool compact);

} // namespace tl::ui::icons

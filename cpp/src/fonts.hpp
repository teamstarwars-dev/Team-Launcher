#pragma once

// Polices de l'interface.
//
// Jusqu'ici le launcher utilisait `AddFontDefault()`, c'est-a-dire ProggyClean
// embarque dans Dear ImGui : une police bitmap de 13 px concue pour les outils
// de debogage. Deux consequences concretes :
//
//  1. **Lisibilite.** Redimensionnee a 24 px pour les titres, une police
//     bitmap devient floue et crenelee. Une police vectorielle est rendue a
//     la bonne taille a chaque corps.
//  2. **Caracteres manquants.** Ses glyphes s'arretent a Latin-1 (U+00FF).
//     Les points de suspension « ... » (U+2026) et les tirets cadratins
//     (U+2014) s'affichaient donc en « ? » sur toutes les pages. Ils avaient
//     du etre remplaces a la main par des ASCII ; la vraie correction est ici.
//
// Aucune police n'est redistribuee avec le launcher : on charge celle du
// systeme. Cela evite toute question de licence, n'ajoute pas un octet au
// binaire, et donne a l'application l'air d'appartenir au bureau sur lequel
// elle tourne. Si rien n'est trouve, on retombe sur ProggyClean : l'interface
// reste utilisable, jamais vide.

#include <string>
#include <vector>

namespace tl::ui::fonts {

// Une police proposee a l'utilisateur.
struct Choice {
    std::string id;    // « auto », ou le chemin du fichier
    std::string label; // ce qui s'affiche dans les parametres
};

// Polices installees sur la machine, dans l'ordre de preference. La premiere
// entree est toujours « auto ». Resultat mis en cache au premier appel.
const std::vector<Choice>& available();

// Chemin reellement retenu pour `id` ("" = aucune, donc ProggyClean).
std::string resolve(const std::string& id);

// (Re)construit l'atlas : corps de base 16 px, plus 24 / 12 / 10 px, le tout
// multiplie par `scale` (borne 0,8 a 1,6). Renseigne fBig, fSmall et fTiny.
// A n'appeler qu'entre deux frames.
void build(const std::string& id, double scale);

// Demande une reconstruction avant la prochaine frame (changement de police
// ou d'echelle depuis les parametres). L'atlas ne peut pas etre modifie au
// milieu d'une frame : la boucle principale consomme ce drapeau.
void request_rebuild();
bool take_rebuild_request();

} // namespace tl::ui::fonts

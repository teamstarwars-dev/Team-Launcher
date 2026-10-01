#pragma once

// Phase 5 — detection de conflits AVANT le lancement.
//
// Le symptome que cela evite : Minecraft se ferme en une seconde avec une
// pile d'exceptions illisible, et il faut ouvrir le journal pour decouvrir
// qu'un seul mod sur quarante etait pour la mauvaise version. Ici on le
// dit avant, en nommant le fichier.
//
// Ce module ne lit rien du reseau et ne modifie rien : il prend la liste
// des mods deja lue par `modmeta` et rend un constat. L'analyse est donc
// pure, et entierement testable.

#include <filesystem>
#include <string>
#include <vector>

#include "modmeta.hpp"

namespace tl::modcheck {

enum class Severity {
    Info,    // remarque (dependance facultative absente)
    Warning, // suspect, le jeu demarrera sans doute
    Error,   // le jeu ne demarrera pas, ou plantera
};

struct Issue {
    Severity sev = Severity::Info;
    std::string file;   // mod concerne (nom de fichier)
    std::string title;  // resume court, affiche en gras
    std::string detail; // explication et, quand c'est possible, la sortie
};

struct Report {
    std::vector<Issue> issues;
    int errors = 0;
    int warnings = 0;
    int infos = 0;
    int analysed = 0;  // mods actifs examines
    int skipped = 0;   // mods desactives, ignores
    // Dependances facultatives absentes, dedoublonnees : c'est la matiere
    // des « suggestions de mods complementaires », qui sortent ainsi des
    // donnees declarees par les mods installes plutot que d'un catalogue
    // invente.
    std::vector<std::string> suggestions;
};

// `loader` et `mcVersion` sont ceux de l'instance ("Vanilla", "Fabric"...).
// Les mods desactives (.jar.disabled) sont comptes mais pas analyses : ils
// ne seront pas charges, leurs incompatibilites ne regardent personne.
Report analyse(const std::vector<modmeta::Mod>& mods, const std::string& loader,
               const std::string& mcVersion);

// Confort : lit le dossier puis analyse.
Report scan(const std::filesystem::path& modsDir, const std::string& loader,
            const std::string& mcVersion);

const char* severity_label(Severity s);

} // namespace tl::modcheck

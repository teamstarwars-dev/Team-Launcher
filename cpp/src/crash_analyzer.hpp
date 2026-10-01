#pragma once

// Diagnostic de crash. Traduit une erreur technique de Minecraft en
// explication claire, avec la marche à suivre — et, depuis la v2, **en
// nommant le mod coupable** quand le rapport permet de le désigner.
//
// Pourquoi c'était la pièce manquante : dire « conflit entre mods » à
// quelqu'un qui en a quarante ne l'avance pas. Dire « le crash vient de
// `webscreen`, fourni par WebDisplay2-2.0.0.jar » règle son problème.
//
// Comment on y arrive sans rien connaître de la machine : **le rapport de
// crash porte lui-même la liste des mods chargés**, avec leur identifiant
// et leur fichier d'origine. On croise cette liste avec les paquets Java
// cités dans la trace de pile. Dans l'exemple ci-dessus, la trace dit
// `at fr.webscreen.registry.ModItems` et la table dit `webscreen` :
// l'intersection désigne le coupable — alors que le nom du fichier,
// « WebDisplay2 », ne contient pas une lettre de « webscreen ».
//
// Conséquence utile : l'analyse est **sans état et sans accès disque**.
// Un log collé depuis n'importe où suffit, ce qui la rend exposable telle
// quelle par une API.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tl::crash {

enum class Cause {
    Unknown,
    OutOfMemory,
    JavaVersion,
    MissingClass,
    Session,
    Graphics,
    Heap,
    FileLocked,
    Network,
    ModConflict,
    ModError, // un mod identifié a lancé l'exception
};

const char* cause_key(Cause c); // identifiant stable, pour une API

// Une entrée de la liste des mods, telle que le rapport la déclare.
struct ModEntry {
    std::string id;
    std::string version;
    std::string source; // nom du .jar
};

// Mod mis en cause, du plus probable au moins.
struct Suspect {
    std::string modId;
    std::string version;
    std::string source;   // .jar d'origine, vide si le rapport ne le dit pas
    std::string evidence; // la ligne qui l'accuse, telle quelle
    int score = 0;        // 0-100, confiance
};

struct Report {
    bool found = false; // une cause OU un suspect
    Cause cause = Cause::Unknown;
    std::string title;  // « Minecraft a manqué de mémoire. »
    std::string action; // « → Augmente la RAM allouée… »
    std::string loader;    // Forge / NeoForge / Fabric / Quilt, si déductible
    std::string mcVersion; // version du jeu, si déductible
    std::string exception; // première ligne d'exception, telle quelle
    std::vector<Suspect> suspects;
    std::string summary; // texte complet prêt à afficher
};

// Analyse complète. Ne lit aucun fichier, ne touche pas au réseau.
Report analyze_report(const std::string& logText);

// Compatibilité : le résumé de `analyze_report`, ou nullopt si rien.
std::optional<std::string> analyze(const std::string& logText);

// Purs, exposés pour les tests. Reconnaissent les trois formats :
//   - table 1.12 Forge : « | LCH | id | version | source | signature |»
//   - Forge/NeoForge modernes : « Mod List: » puis « fichier |Nom |id |ver »
//   - Fabric : « Fabric Mods: » puis « id: Nom version »
std::vector<ModEntry> parse_mod_list(const std::string& logText);

// Identifiants cités dans la trace de pile, dans l'ordre d'apparition.
std::vector<std::string> stack_tokens(const std::string& logText);

// Analyse les journaux d'une instance : crash-reports/ récent (< 5 min)
// puis les 300 dernières lignes de game-log.txt.
std::optional<std::string> analyze_instance(const std::filesystem::path& gameDir);
Report analyze_instance_report(const std::filesystem::path& gameDir);

// n dernières lignes d'un fichier texte. "" si absent.
std::string tail_lines(const std::filesystem::path& file, int n);

} // namespace tl::crash

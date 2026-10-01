#pragma once

// Developpement de mods — portage de ModDevPage.cs : creation d'un squelette
// de projet (Fabric / Forge / NeoForge / Bedrock), build et execution via
// Gradle, console de sortie.
//
// CORRECTIFS vs C# (ModDevPage.cs) :
//
//  1. **Le C# ne generait jamais le wrapper Gradle.** `BuildProjectAsync` et
//     `RunProjectAsync` cherchaient `gradlew.bat` dans le dossier du projet et
//     affichaient « gradlew.bat non trouve. Cree le projet d'abord. » — or
//     creer le projet ne le produisait pas. Build et Run ne pouvaient donc
//     JAMAIS fonctionner sur un projet cree par le launcher. Ici la chaine
//     d'outils est detectee (`detect_toolchain`) : wrapper du projet, sinon
//     `gradle` du PATH, et le message dit exactement ce qui manque.
//  2. Les versions de Yarn, du loader Fabric et de la Fabric API etaient
//     devinees (`yarn:<mc>+build.1`, `fabric-api:<mc>+`), des chaines qui
//     n'existent pas la plupart du temps : le premier build echouait a la
//     resolution des dependances. Elles sont ici **resolues en ligne** contre
//     `meta.fabricmc.net` (deja dans l'allowlist), avec repli documente.
//  3. Aucune validation du nom ni du package : un nom avec accent ou espace
//     produisait une classe Java invalide et un `mod_id` refuse par Fabric.
//     `mod_id_from`, `class_name_from` et `valid_package` normalisent.
//  4. Le C# ecrivait dans le dossier indique sans verifier qu'il est vide :
//     un projet existant etait ecrase en silence. Ici les fichiers deja
//     presents ne sont pas remplaces sans `overwrite`.
//  5. `LogConsole` colorait sur la presence de « ERROR » n'importe ou dans la
//     ligne. Conserve (utile), mais la detection de succes ne repose plus sur
//     un emoji : c'est le code de sortie du processus qui fait foi.

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tl::moddev {

enum class Loader { Fabric, Forge, NeoForge, Bedrock };

const char* loader_name(Loader l);
// "fabric" -> Loader::Fabric ; Fabric par defaut.
Loader loader_from(const std::string& s);

// Versions de Minecraft proposees par chargeur (table interne, comme le C#).
const std::vector<std::string>& versions_for(Loader l);

struct ProjectSpec {
    Loader loader = Loader::Fabric;
    std::string mcVersion = "1.21.4";
    std::string name;     // « Mon Mod »
    std::string pkg;      // « com.exemple.monmod »
    std::filesystem::path dir;
};

// --- Normalisations (pures) -------------------------------------------------

// Identifiant de mod : minuscules, [a-z0-9_-], commence par une lettre.
// "" si rien d'exploitable.
std::string mod_id_from(const std::string& name);

// Nom de classe Java : PascalCase, identifiant valide, accents retires.
// "MonMod" par defaut si rien d'exploitable.
std::string class_name_from(const std::string& name);

// Nom de paquet Java : segments [a-z][a-z0-9_]* separes par des points, et
// aucun mot reserve du langage.
bool valid_package(const std::string& pkg);

// UUID v4 pour les manifestes Bedrock (le C# utilisait Guid.NewGuid).
std::string uuid_v4();

// --- Versions des dependances ----------------------------------------------

struct Deps {
    std::string yarn;        // ex. « 1.21.4+build.8 »
    std::string loader;      // ex. « 0.16.9 »
    std::string fabricApi;   // ex. « 0.114.0+1.21.4 »
    std::string forge;       // ex. « 1.21.4-54.0.16 »
    std::string neoforge;    // ex. « 21.4.50-beta »
    bool resolved = false;   // false = valeurs de repli
    std::string note;        // explication si repli
};

// Resout les versions reelles pour Fabric contre meta.fabricmc.net. Hors
// ligne ou en cas de refus, renvoie un repli et renseigne `note`.
Deps resolve_deps(Loader l, const std::string& mcVersion);

// --- Generation -------------------------------------------------------------

// Fichiers du projet : chemin RELATIF -> contenu. Fonction pure, testable
// sans toucher au disque.
std::map<std::string, std::string> project_files(const ProjectSpec& spec,
                                                 const Deps& deps);

using Log = std::function<void(const std::string&)>;

struct CreateResult {
    bool ok = false;
    std::string error;
    std::vector<std::string> written;  // chemins relatifs ecrits
    std::vector<std::string> kept;     // deja presents, laisses en place
};

// Ecrit le squelette. `overwrite` remplace les fichiers deja presents.
CreateResult create_project(const ProjectSpec& spec, const Deps& deps,
                            bool overwrite, const Log& log);

// --- Chaine d'outils --------------------------------------------------------

struct Toolchain {
    bool wrapper = false;         // gradlew.bat present dans le projet
    bool gradleOnPath = false;    // gradle(.bat) trouve dans le PATH
    bool gradleManaged = false;   // Gradle telecharge par le launcher
    // Un JRE a bien un `java`, mais pas de `javac` : il ne compile rien.
    // Distinguer les deux evite d'annoncer « JDK oui » a quelqu'un qui ne
    // pourra pas construire son mod.
    bool jdk = false;             // un javac a ete trouve
    bool javaPresent = false;     // un java a ete trouve (JRE ou JDK)
    int javaMajor = 0;            // 8, 17, 21... 0 = inconnu
    int javaNeeded = 0;           // ce que le chargeur choisi reclame
    std::string javaHome;         // racine du JDK retenu, "" si aucun
    std::string javaVersion;      // ce que « java -version » a repondu
    std::string command;          // ce qui sera lance ; "" si rien d'utilisable
    std::string problem;          // ce qui manque, en clair
};

// `mcVersion` sert a dire quel JDK est necessaire (21 depuis 1.20.5, 17
// depuis 1.18, 8 avant). Vide = on ne se prononce pas sur la version.
Toolchain detect_toolchain(const std::filesystem::path& projectDir,
                           const std::string& mcVersion = {});

// JDK requis par cette version de Minecraft.
int jdk_major_for(const std::string& mcVersion);

// --- Installation automatique de la chaine d'outils -------------------------
// La page proposait « genere le wrapper : gradle wrapper ... » — un
// conseil impossible a suivre, puisqu'il faut deja Gradle pour le lancer.
// Le launcher sait telecharger un JRE pour jouer ; il sait desormais
// telecharger Gradle et un JDK pour construire.

// Gradle gere par le launcher (<runtime>/gradle/...). Rend le chemin de
// l'executable, ou "" avec errOut renseigne. Ne retelecharge pas si
// l'installation est deja la.
std::string ensure_gradle(const Log& log, const std::atomic<bool>* cancel,
                          std::string* errOut = nullptr);

// Chemin de Gradle gere s'il est deja installe, "" sinon. Ne telecharge
// rien : sert a la detection.
std::string managed_gradle_path();

// --- Execution --------------------------------------------------------------

// Lance une ligne de commande dans `dir`, remonte chaque ligne de sortie au
// fil de l'eau (stdout et stderr fusionnes). Renvoie le code de sortie, ou
// -1 si le processus n'a pas pu demarrer. `cancel` tue le processus.
int run(const std::filesystem::path& dir, const std::string& commandLine,
        const Log& log, const std::atomic<bool>* cancel = nullptr);

} // namespace tl::moddev

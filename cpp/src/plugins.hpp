#pragma once

// Phase 8 — plugins internes.
//
// DÉCISION DE CONCEPTION, et la plus importante du module : un plugin
// n'est **pas** une bibliothèque native chargée dans le launcher. C'est un
// dossier contenant un `plugin.json` qui déclare des commandes, exécutées
// comme des **processus séparés**.
//
// Charger du code natif aurait signifié : un plugin mal compilé fait
// planter tout le launcher ; l'ABI C++ interdit de mélanger des binaires
// construits avec d'autres compilateurs ou d'autres versions ; et un
// plugin aurait eu accès à la mémoire du processus, donc aux jetons de
// session Microsoft. Aucun de ces trois problèmes n'a de bonne réponse.
// Un processus séparé en a une pour les trois.
//
// SÉCURITÉ — à lire avant de s'en servir. Un plugin lance des programmes
// avec les droits de l'utilisateur. Il n'y a pas de bac à sable, et ce
// module n'en promet aucun. Les garde-fous sont donc ceux-ci :
//
//   - **Rien ne s'active tout seul.** Déposer un dossier dans `plugins/`
//     le rend visible, pas actif. Il faut l'autoriser nommément, et la
//     liste des plugins autorisés vit dans la configuration.
//   - **La commande exacte est montrée avant l'autorisation**, telle
//     qu'elle sera exécutée. On n'autorise pas un nom, on autorise une
//     ligne de commande qu'on a lue.
//   - **Pas de shell.** Le programme et ses arguments sont passés
//     séparément au système : aucune interprétation de `&&`, `|`, `>` ou
//     de guillemets, donc aucune injection par un argument.
//   - **Un plugin modifié redevient interdit.** L'autorisation porte sur
//     l'empreinte du manifeste, c'est-à-dire sur un contenu et non sur un
//     nom : si `plugin.json` change, il faut réautoriser. Sans cela,
//     autoriser une fois reviendrait à signer un chèque en blanc pour
//     tout ce que le fichier dirait plus tard.
//     Corollaire assumé : retrouver **exactement** le manifeste approuvé
//     le réautorise. Ce sont les octets que l'utilisateur a lus et
//     acceptés ; redemander son accord pour les mêmes serait du bruit.
//     Retirer l'autorisation, lui, efface toutes les empreintes du
//     plugin, et aucun retour en arrière ne la ressuscite.

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::plugins {

// Où l'action apparaît dans l'interface.
enum class Where {
    Instance, // page détail d'une instance (a besoin d'une instance)
    Tools,    // liste générale, sans instance
};

struct Action {
    std::string label;
    Where where = Where::Tools;
    std::string program;             // exécutable, jamais une ligne de shell
    std::vector<std::string> args;   // avec des substituables
    std::string workDir;             // vide = dossier du plugin
};

struct Plugin {
    std::string id;          // nom du dossier : ce qui identifie le plugin
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::filesystem::path dir;
    std::vector<Action> actions;
    std::string manifestHash; // SHA-1 du plugin.json lu
    std::string error;        // non vide = manifeste refusé, voir le texte
    bool enabled = false;     // autorisé ET empreinte inchangée
};

// Dossier des plugins (<données>/plugins). Créé à la demande.
std::filesystem::path dir();

// Pur (testable) : interprète un manifeste. `id` est le nom du dossier.
// Un manifeste incompréhensible ressort avec `error` renseigné plutôt que
// d'être ignoré en silence — un plugin qui n'apparaît pas est un plugin
// qu'on croit cassé pour une autre raison.
Plugin parse_manifest(const std::string& json, const std::string& id);

// Tous les plugins trouvés, triés par nom. `enabled` est calculé à partir
// de la configuration ET de l'empreinte.
std::vector<Plugin> list();

// Autorise / retire l'autorisation. Autoriser enregistre l'empreinte
// courante : modifier ensuite le manifeste la révoque de fait.
void set_enabled(const Plugin& p, bool on);

// Contexte de substitution. Les clés absentes laissent le substituable
// tel quel plutôt que de le remplacer par du vide : une commande qui
// garde `{instanceDir}` en clair est visiblement fausse, une commande qui
// reçoit une chaîne vide peut agir sur le mauvais dossier.
struct Context {
    std::string instanceId;
    std::string instanceName;
    std::string instanceDir;
    std::string gameVersion;
    std::string dataDir;
    std::string pluginDir;
};

// Pur (testable) : remplace {instanceDir}, {instanceId}, {instanceName},
// {gameVersion}, {dataDir}, {pluginDir}.
std::string expand(const std::string& s, const Context& ctx);

// Ligne de commande telle qu'elle sera exécutée, pour l'afficher avant
// d'autoriser. Les arguments contenant une espace sont entourés de
// guillemets — c'est de l'affichage, pas ce qui est passé au système.
std::string preview(const Action& a, const Context& ctx);

// Lance l'action. Refuse si le plugin n'est pas autorisé.
// false + errOut sinon.
bool run(const Plugin& p, const Action& a, const Context& ctx,
         std::string* errOut = nullptr);

} // namespace tl::plugins

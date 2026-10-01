#pragma once

// Phase 5 — lecture des metadonnees d'un mod.
//
// Un .jar de mod declare qui il est dans un fichier de son archive, dont
// le nom et le format dependent du chargeur :
//   - Fabric   : fabric.mod.json          (JSON)
//   - Quilt    : quilt.mod.json           (JSON, structure differente)
//   - Forge    : META-INF/mods.toml       (TOML)
//   - NeoForge : META-INF/neoforge.mods.toml (idem, nouveau nom depuis 1.20.5)
//   - ancien   : mcmod.info               (JSON, <= 1.12)
//
// C'est la seule source fiable : le nom de fichier ne dit rien. « Sodium
// 0.5.8 pour 1.20.1 » peut s'appeler `sodium.jar` et avoir ete telecharge
// pour une autre version.
//
// Tout ce qui se parse est expose separement du .jar : les analyseurs sont
// purs, donc testables sans fabriquer d'archive.

#include <filesystem>
#include <string>
#include <vector>

namespace tl::modmeta {

enum class Loader { Unknown, Fabric, Quilt, Forge, NeoForge };

const char* loader_name(Loader l);
Loader loader_from_string(const std::string& s);

// Dependance declaree par un mod.
struct Dep {
    std::string id;       // modId attendu
    bool mandatory = true;
    std::string range;    // contrainte brute, vide = n'importe laquelle
};

struct Mod {
    std::string file;     // nom du fichier, tel qu'il est sur le disque
    std::string id;       // modId ; vide = metadonnees illisibles
    std::string name;     // nom affichable (retombe sur l'id, puis le fichier)
    std::string version;
    Loader loader = Loader::Unknown;
    std::string mcRange;  // contrainte sur Minecraft, brute
    std::vector<Dep> deps;
    bool disabled = false;   // .jar.disabled
    std::string readError;   // non vide = archive illisible ou sans manifeste
};

// Lit le .jar (ou .jar.disabled). Ne lance jamais : une archive corrompue
// ressort avec `readError` renseigne, et l'appelant decide quoi en dire.
Mod read_jar(const std::filesystem::path& jar);

// Tous les mods d'un dossier, actifs et desactives, tries par nom de
// fichier. Dossier absent = liste vide.
std::vector<Mod> read_dir(const std::filesystem::path& modsDir);

// --- Analyseurs purs -------------------------------------------------------
// Chacun renseigne ce qu'il trouve et laisse le reste vide. `readError` est
// renseigne si le document est illisible.

Mod parse_fabric(const std::string& json);
Mod parse_quilt(const std::string& json);
Mod parse_mcmod_info(const std::string& json);
// `neoforge` ne change pas la grammaire, seulement le chargeur rapporte.
Mod parse_mods_toml(const std::string& toml, bool neoforge);

// --- Versions --------------------------------------------------------------

// Compare deux versions « a.b.c[-suffixe] » : <0, 0, >0. Les composantes
// non numeriques sont comparees en texte, et une version SANS suffixe est
// consideree superieure a la meme AVEC (« 1.0 » > « 1.0-beta1 »), comme le
// veut semver.
int compare_versions(const std::string& a, const std::string& b);

// `version` satisfait-elle `range` ? Formes acceptees :
//   ""  ou  "*"            n'importe laquelle
//   "1.20.1"               exactement (Fabric l'entend ainsi)
//   ">=1.20" "<1.21" "<=" ">" "="
//   "~1.20.1"              meme mineure (>= 1.20.1, < 1.21)
//   "^1.20.1"              meme majeure (>= 1.20.1, < 2)
//   "[1.20,1.21)"          intervalle Maven (Forge), bornes [ ] ( )
//   "[1.20.1]"             intervalle Maven ponctuel
//   plusieurs contraintes separees par des espaces ou des virgules : ET
//   plusieurs alternatives separees par « || » : OU
// Une contrainte qu'on ne sait pas lire est acceptee : refuser de lancer
// sur une syntaxe exotique serait pire que ne rien dire.
bool version_matches(const std::string& version, const std::string& range);

} // namespace tl::modmeta

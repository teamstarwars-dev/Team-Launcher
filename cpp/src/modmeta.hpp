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
    // Manifeste effectivement lu : « neoforge.mods.toml », « quilt.mod.json »,
    // « fabric.mod.json », « mods.toml » ou « mcmod.info ». Sa semantique
    // compte : dans mcmod.info, « 1.12 » designe toute la branche 1.12.
    std::string source;
    // Le manifeste lu n'est PAS un format que le chargeur de l'instance
    // sait lire (ex. un mods.toml dans une instance Forge 1.12, qui ne
    // connait que mcmod.info). Ce qu'il declare n'engage donc rien.
    bool foreign = false;
};

// Lit le .jar (ou .jar.disabled). Ne lance jamais : une archive corrompue
// ressort avec `readError` renseigne, et l'appelant decide quoi en dire.
//
// `legacyForge` : l'instance est en Forge 1.12 ou anterieur. Ces versions
// ne lisent QUE mcmod.info (mods.toml n'existe que depuis 1.13). Or de
// nombreux jars 1.12.2 embarquent aussi un mods.toml pour une version
// plus recente — le lire en priorite faisait declarer « incompatible »
// un mod parfaitement prevu pour l'instance. En mode legacyForge,
// mcmod.info passe donc en premier, et un jar qui n'en a pas est marque
// `foreign`.
Mod read_jar(const std::filesystem::path& jar, bool legacyForge = false);

// Tous les mods d'un dossier, actifs et desactives, tries par nom de
// fichier. Dossier absent = liste vide.
std::vector<Mod> read_dir(const std::filesystem::path& modsDir,
                          bool legacyForge = false);

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

// --- Identifiant de version de Minecraft ---------------------------------
//
// La version d'une instance etait un champ texte libre, enregistre tel
// quel. « 112.2 » (point oublie) passait donc sans un mot : la
// verification d'avant-lancement signalait ensuite chacun des 39 mods
// comme incompatible, et l'installation aurait echoue chez Mojang.
//
// Controle de FORME, hors ligne : on ne fait pas d'appel reseau a chaque
// enregistrement d'un formulaire. Formes acceptees :
//   latest                      — derniere version
//   1.21  1.21.4                — versions historiques
//   26.1  26.1.2                — numerotation par annee (depuis 2026)
//   1.21-pre1  1.20.5-rc1       — pre-versions
//   26.1-snapshot-3             — instantanes au nouveau format
//   24w14a                      — instantanes hebdomadaires
//   b1.7.3  a1.0.4  c0.0.13a    — anciennes alpha / beta / classic
//   rd-132211  inf-20100618     — pre-classic, infdev
// L'annee doit etre plausible (25 a 99) : « 112.2 » n'est ni une version
// historique ni une annee.
bool is_mc_version(const std::string& v);

// Correction probable d'une saisie invalide, ou vide si on n'en voit pas.
// Vise la faute la plus courante : le point oublie apres le « 1 » initial
// (« 112.2 » -> « 1.12.2 », « 1201 » -> rien : trop ambigu).
std::string suggest_mc_version(const std::string& v);

} // namespace tl::modmeta

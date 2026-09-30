#pragma once

// Profils de lancement par instance.
//
// Une meme instance sert a plusieurs usages : une partie competitive veut
// le moins de mods possible et peu de RAM, une partie tranquille veut les
// shaders et la carte. Aujourd'hui il faut activer et desactiver les mods
// un par un a chaque fois, et retoucher la memoire dans les reglages.
//
// Un profil retient trois choses :
//   - la memoire allouee (0 = on garde celle de l'instance) ;
//   - les arguments JVM ("" = ceux de l'instance) ;
//   - l'ensemble des mods DESACTIVES, s'il a ete capture.
//
// Le troisieme point s'appuie sur le meme mecanisme que la page Mods : un
// mod desactive porte le suffixe `.disabled`. Appliquer un profil renomme
// donc des fichiers — d'ou deux precautions : on ne touche a rien tant que
// l'utilisateur n'a pas capture un etat, et un mod inconnu du profil est
// laisse tel quel plutot que suppose actif (un mod installe depuis la
// capture ne doit pas disparaitre en silence).

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace tl::presets {

struct Preset {
    std::string name;
    int ramGb = 0;            // 0 = valeur de l'instance / globale
    std::string jvmArgs;      // "" = celle de l'instance
    bool hasMods = false;     // un etat de mods a ete capture
    std::vector<std::string> disabled; // noms de base des mods desactives
};

// Lecture / ecriture dans le JSON d'instance (cle « Presets »).
std::vector<Preset> load(const nlohmann::json& inst);
void store(nlohmann::json& inst, const std::vector<Preset>& v);

// Nom du profil actif, "" si aucun.
std::string active(const nlohmann::json& inst);
void set_active(nlohmann::json& inst, const std::string& name);

// Etat de mods courant du dossier : noms de base de ceux qui sont
// DESACTIVES. Sert a capturer un profil depuis ce qui est en place.
std::vector<std::string> current_disabled(const std::filesystem::path& modsDir);

struct ApplyResult {
    int enabled = 0;   // mods reactives
    int disabled = 0;  // mods desactives
    int untouched = 0; // presents mais absents du profil : laisses tels quels
    int failed = 0;    // renommage impossible (fichier verrouille...)
};

// Aligne le dossier sur l'ensemble `disabled` du profil. Les mods qui ne
// figurent NI comme actifs NI comme desactifs dans le profil ne sont pas
// touches : ils ont ete ajoutes apres la capture.
ApplyResult apply_mods(const std::filesystem::path& modsDir,
                       const std::vector<std::string>& disabled,
                       const std::vector<std::string>& known);

// Noms de base de tous les mods du dossier, actifs ou non.
std::vector<std::string> all_mods(const std::filesystem::path& modsDir);

} // namespace tl::presets

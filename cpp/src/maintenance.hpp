#pragma once

// Portage de HealthService.cs, CleanupService (ServerPing.cs) et de la partie
// portable d'UpdateService.cs / UpdateChecker.cs.

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::health {

struct Check {
    std::string name;
    bool ok = false;
    std::string detail;
};

// Diagnostic complet (bloquant : une requete reseau de 6 s au plus).
std::vector<Check> run_all(const std::atomic<bool>* cancel = nullptr);

} // namespace tl::health

namespace tl::cleanup {

struct Result {
    int files = 0;
    double mb = 0.0;
};

// Installeurs Forge/NeoForge et archives JRE du dossier runtime, plus les .zip
// de plus de 30 jours du dossier de donnees.
Result run();

} // namespace tl::cleanup

namespace tl::updates {

// DIVERGENCE vs C# (UpdateService.cs) : le C# deleguait telechargement,
// verification de signature et remplacement atomique a Velopack
// (ApplyUpdatesAndRestart). Velopack n'etant pas portable tel quel, ce port
// implemente l'equivalent robuste sans dependance :
//   1. check() lit l'API GitHub Releases (comme ici avant) ;
//   2. download_update() stage le zip de l'OS courant a cote (dossier
//      updates/) via tl::http::get_to_file (progression + annulation) ;
//   3. install_staged_and_restart() deploie puis relance : sous Windows via un
//      script .bat APRES la sortie du launcher (renommage de l'exe en cours
//      impossible), sous Linux en process (l'exe n'est pas verrouille) +
//      relance detachee (double fork).
// Le marqueur updates/pending.json (« mise a jour en attente ») survit a un
// redemarrage : un staged telecharge reste proposable au prochain demarrage,
// AVANT l'UI (voir extrait main.cpp fourni en reponse).
// Verif d'integrite : le C# n'en faisait aucune explicite (signatures
// Velopack internes). Ici : HTTPS seul (aucun contournement TLS) + controle
// de la taille recue vs taille annoncee par l'API GitHub quand disponible.

// Version compilee (TL_VERSION_STRING).
const char* current_version();

// Comment le launcher est installe sur cette machine. Depuis l'ajout de
// l'installeur Inno Setup (27/09/2026) les deux cas ne se mettent pas a jour
// de la meme facon :
//   - Portable  : dossier autonome, la mise a jour remplace les fichiers.
//   - Installed : pose par l'installeur, la mise a jour relance le Setup, qui
//                 sait fermer l'application, ecrire dans Program Files et
//                 tenir a jour l'entree « Applications installees ».
// Remplacer les fichiers a la main sous Program Files echouerait faute de
// droits, et laisserait la version affichee par Windows perimee.
enum class Deploy { Portable, Installed };

// Detection : sous Windows, presence de `unins000.exe` a cote de
// l'executable (l'installeur l'y depose toujours). Un build portable ou
// lance depuis le dossier de compilation est donc Portable.
Deploy deployment();

// Le dossier d'installation est-il inscriptible par le processus courant ?
// Faux pour un Program Files sans elevation : la mise a jour doit alors
// passer par une demande UAC.
bool install_dir_writable();

struct Info {
    std::string version; // tag sans le « v » initial
    std::string notes;   // corps de la release
    std::string url;     // page de la release
    std::string assetUrl;  // zip de l'OS courant choisi (vide = aucun)
    std::string assetName; // nom de fichier de l'asset
    long long assetSize = -1; // taille annoncee (-1 = inconnue)
};

// Mise a jour telechargee en attente d'installation.
struct Staged {
    Info info;
    std::filesystem::path file; // zip stage
};

using ProgressFn = std::function<void(long long done, long long total)>;

// Derniere release GitHub. nullopt = deja a jour, ou verification impossible
// (errOut renseigne dans ce second cas).
std::optional<Info> check(std::string* errOut = nullptr);

// Compare deux versions « a.b.c[.d] » : <0, 0, >0.
int compare_versions(const std::string& a, const std::string& b);

// Pur (testable hors ligne) : choisit l'asset zip Windows x64 d'une release
// GitHub (tableau « assets »). Retourne l'URL (vide = aucun zip Windows) et
// renseigne nom/taille quand disponibles.
std::string select_asset_url(const nlohmann::json& release,
                             std::string* nameOut = nullptr,
                             long long* sizeOut = nullptr);

// Pur (testable hors ligne) : interprete le corps JSON de /releases/latest
// (objet) ou de /releases (tableau, canal beta). Sur un tableau : les
// brouillons sont ignores, les preversions ne sont retenues qu'en canal
// beta, et c'est la plus HAUTE version qui gagne — pas la plus recemment
// publiee. nullopt sans errOut = pas plus recent que current_version().
std::optional<Info> parse_release_json(const std::string& body,
                                       std::string* errOut = nullptr);

// Dossier de staging (<donnees>/updates).
std::filesystem::path updates_dir();

// Marqueur « mise a jour en attente » (nullopt = rien de pret).
std::optional<Staged> staged();
bool has_staged();
bool clear_staged(std::string* errOut = nullptr);

// Telecharge l'asset vers updates/ + ecrit le marqueur. Progression et
// annulation via tl::http. false = echec ou annulation (errOut renseigne).
bool download_update(const Info& info, ProgressFn progress,
                     const std::atomic<bool>* cancel,
                     std::string* errOut = nullptr);

// Genere updates/apply-update.bat (attente de sortie, deploiement,
// suppression du marqueur, relance), le lance en detache et rend la main :
// l'appelant doit quitter proprement juste apres (true = script lance).
bool install_staged_and_restart(std::string* errOut = nullptr);

} // namespace tl::updates

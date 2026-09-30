#pragma once

// Phase 7 — lancement du launcher a l'ouverture de la session.
//
// Deux mecanismes, un par plateforme, choisis pour ne demander aucun droit
// d'administrateur et n'ecrire que dans le profil de l'utilisateur :
//   - Windows : valeur « TeamLauncher » sous HKCU\...\CurrentVersion\Run ;
//   - Linux   : ~/.config/autostart/teamlauncher.desktop (freedesktop).
//
// Le launcher est alors demarre avec `--autostart`, que main() interprete
// comme « demarre reduit » : arriver au premier plan par surprise a chaque
// ouverture de session serait insupportable.
//
// L'etat n'est PAS deduit du reglage : il est relu a la source. Un reglage
// coche alors que la cle a disparu (profil recopie, nettoyeur de demarrage,
// politique d'entreprise) mentirait a l'utilisateur.

#include <string>

namespace tl::startup {

// Faux si le mecanisme n'existe pas sur cette plateforme : l'interface grise
// alors la case plutot que de promettre ce qu'elle ne tiendra pas.
bool autostart_supported();

// Etat reel (registre / fichier), pas la valeur du reglage.
bool autostart_enabled();

// Ecrit ou retire l'entree. false + errOut en cas d'echec.
bool set_autostart(bool on, std::string* errOut = nullptr);

// Chemin de l'executable courant ("" si introuvable).
std::string exe_path_utf8();

} // namespace tl::startup

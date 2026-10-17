#pragma once

// Import AUTOMATIQUE des instances de l'application CurseForge au démarrage.
//
// L'utilisateur n'a plus à importer chaque instance à la main : on scanne le
// dossier d'instances de l'app CurseForge (réglage, sinon chemin par défaut
// <profil>/curseforge/minecraft/Instances), et on copie dans le launcher
// celles qui n'y sont pas encore. Le repérage se fait par un champ « CfSource »
// posé sur l'instance (chemin d'origine) : une instance déjà importée est
// ignorée, même après un redémarrage.
//
// La copie (potentiellement volumineuse) tourne dans une tâche de fond. Les
// instances prêtes sont déposées dans une file que l'UI vide sur SON fil, pour
// ne pas muter la liste des instances pendant que l'interface la lit.

#include <nlohmann/json.hpp>

#include <vector>

namespace tl::cfimport {

// Lance (une seule fois) le scan + import asynchrone. Sans effet si l'option
// est désactivée ou si déjà lancé. À appeler au démarrage, depuis le fil UI.
void start_async();

// À appeler sur le FIL UI (frame loop) : récupère et vide les instances
// importées prêtes à être ajoutées à la liste. Vide la plupart du temps.
std::vector<nlohmann::json> take_ready();

} // namespace tl::cfimport

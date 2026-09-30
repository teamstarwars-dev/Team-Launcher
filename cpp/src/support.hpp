#pragma once

// Phase 7 — assistance : rapport systeme et export des journaux.
//
// Objectif : qu'un utilisateur puisse joindre a un ticket UN fichier qui
// contient ce dont on a besoin pour comprendre, et rien de ce qu'on n'a pas
// a savoir. Le config.json est donc expurge avant d'entrer dans l'archive :
// il porte le jeton de session Microsoft, la cle CurseForge, l'URL du
// webhook... tous chiffres sur le disque, mais un ticket public n'est pas
// l'endroit ou tenter le sort.

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace tl::support {

// Liens d'entraide (le salon Discord vient des reglages, il peut etre vide).
std::string help_center_url();
std::string ticket_url();
std::string suggestion_url();

// Rapport lisible : version, plateforme, deploiement, RAM, chemins,
// reglages non sensibles. Aucune donnee personnelle au-dela du dossier de
// donnees, qui contient le nom de compte Windows — inevitable, et utile.
std::string system_report();

// Pur (teste hors ligne) : remplace la valeur de toute cle sensible par
// « (masqué) ». Recursif, et sur les tableaux aussi : PteroHosts est un
// tableau d'objets qui portent des cles d'API.
nlohmann::json redacted_config(const nlohmann::json& raw);

// Vrai si le nom de cle designe une valeur a ne pas exporter.
bool is_sensitive_key(const std::string& key);

// Ecrit une archive zip : rapport, journal, config expurge.
// false + errOut en cas d'echec.
bool export_logs(const std::filesystem::path& zipPath, std::string* errOut);

} // namespace tl::support

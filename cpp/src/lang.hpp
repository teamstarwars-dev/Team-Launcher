#pragma once

// Portage de Lang.cs — bilingue fr/en.
//
// Deux mecanismes, comme le C# :
//   t("texte français")             -> dictionnaire FrToEn (equivalent Lang.Apply)
//   t("texte français", "english")  -> paire explicite    (equivalent Lang.T)
//
// En mode immediat (ImGui) tout est redessine a chaque frame : changer la
// langue s'applique instantanement, sans redemarrage (contrairement au C#
// qui devait relancer l'exe ou reconstruire les pages).

#include <string>

namespace tl::lang {

// "fr" (defaut) ou "en", lu depuis DataStore::settings.language.
const char* current();
bool is_en();

// Ecrit la langue dans les reglages (DataStore::save()) — prise en compte
// des la frame suivante.
void set_language(const char* code);

// Paire explicite (Lang.T).
const char* t(const char* fr, const char* en);

// Dictionnaire : rend la traduction si elle existe, sinon le francais.
const char* t(const char* fr);
std::string t(const std::string& fr);

// Nombre d'entrees du dictionnaire (tests).
std::size_t dict_size();

} // namespace tl::lang

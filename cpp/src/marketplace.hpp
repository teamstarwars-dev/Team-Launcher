#pragma once
// V8 — marketplace : catalogue, installation, liste de révocation.
//
// Le serveur n'est PAS une source de confiance. Il sert des archives ; ce
// qui décide, c'est la signature, vérifiée ici avec les racines compilées
// dans le launcher. Un serveur compromis peut refuser de servir, il ne
// peut pas faire installer un fichier que la chaîne de confiance n'a pas
// signé :
//   - l'archive est lue EN MÉMOIRE et extraite dans un dossier de travail
//     vide, avec les mêmes refus que le serveur (chemins, liens, doublons de
//     casse, bombes) ;
//   - la signature est vérifiée sur ce dossier AVANT toute installation ;
//     « non signé » est un refus, mode développeur ou pas : le mode
//     développeur sert aux plugins qu'on écrit soi-même, pas à ce qu'on
//     télécharge ;
//   - l'installation est un renommage, rien n'est à moitié copié ;
//   - un plugin installé n'est PAS autorisé : l'utilisateur lit ses
//     permissions et l'autorise lui-même, comme tout plugin.

#include "signing.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tl::market {

struct Entry {
    signing::Kind kind = signing::Kind::Plugin;
    std::string slug;    // identifiant : nom du dossier d'un plugin
    std::string name;
    std::string version;
    std::string description;
    std::vector<std::string> permissions;
};

// Adresse du serveur, normalisée (sans « / » final), ou nullopt si elle
// est absente ou refusée : HTTPS obligatoire, sauf boucle locale ; pas
// d'identifiants, de requête ni de fragment dans l'URL.
std::optional<std::string> normalize_base(const std::string& url, std::string* err);

// Pur (testable). Le catalogue est une donnée du réseau : chaque entrée
// est validée, une entrée invalide est écartée sans faire tomber le reste.
std::vector<Entry> parse_catalog(const std::string& json, std::string* err);

// Extraction stricte d'une archive en mémoire dans `dest`, qui doit être
// absent ou vide. false + err sinon (et `dest` est alors supprimé).
bool extract_package(const std::string& zipBytes, const std::filesystem::path& dest,
                     std::string* err);

struct Result {
    bool ok = false;
    std::string message; // à afficher tel quel
};

// Cœur de l'installation, sans réseau (testable) : extrait, vérifie la
// signature avec `roots`/`revs`, puis installe.
//   plugin : dans <targetDir>/<slug>, en remplaçant une version précédente ;
//   mod    : l'archive ne doit contenir que des .jar à sa racine, copiés
//            dans <targetDir> (dossier mods d'une instance) ; un fichier
//            déjà présent n'est jamais écrasé.
// `workDir` : dossier de travail, sur le même volume que targetDir.
Result install_from_zip(const std::string& zipBytes, const Entry& e,
                        const std::filesystem::path& targetDir,
                        const std::filesystem::path& workDir, const signing::Roots& roots,
                        const signing::Revocations* revs, std::int64_t now);

// --- Réseau (adresse lue dans la configuration) ----------------------------

std::optional<std::vector<Entry>> fetch_catalog(std::string* err,
                                                const std::atomic<bool>* cancel = nullptr);

// Télécharge puis install_from_zip avec les racines compilées et la liste
// de révocation en vigueur. Plugin : targetDir ignoré (dossier des plugins).
Result install(const Entry& e, const std::filesystem::path& modsDir,
               const std::atomic<bool>* cancel = nullptr);

// Récupère et installe la liste de révocation (anti-retour arrière par seq,
// voir signing::install_revocations). true si une liste valide a été lue
// (même si ce n'était pas une nouvelle), ou si le serveur n'en a publié
// aucune.
bool refresh_revocations(std::string* err, const std::atomic<bool>* cancel = nullptr);

// Envoie la télémétrie anonyme en attente (plugin_telemetry.hpp). Rien
// n'est envoyé si l'utilisateur ne l'a pas activée. Échec réseau ou
// serveur occupé : les compteurs sont remis en attente.
bool send_telemetry(std::string* err, const std::atomic<bool>* cancel = nullptr);

} // namespace tl::market

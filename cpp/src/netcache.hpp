#pragma once

// Cache disque des reponses de metadonnees (CurseForge, Modrinth, Mojang).
//
// Le launcher redemande sans arret les memes choses : la liste des versions
// d'un mod a chaque ouverture de sa fiche, le catalogue a chaque retour sur
// la page Exploration, les memes recherches d'une session a l'autre. Chaque
// aller-retour coute une seconde ou deux, et CurseForge comme Modrinth
// limitent le debit par cle.
//
// Deux usages, pas un seul :
//
//  1. **Eviter les appels repetes.** Une reponse de moins de `maxAge` est
//     servie depuis le disque, sans reseau.
//  2. **Mode hors-ligne.** Quand le reseau echoue, on sert la copie meme
//     PERIMEE plutot que rien. Une liste de mods vieille d'une semaine vaut
//     mieux qu'une page vide, a condition de le DIRE — d'ou le drapeau
//     `stale` du resultat, que l'interface affiche.
//
// Ne convient qu'aux metadonnees publiques. Rien de personnel ni
// d'authentifie ne doit passer par ici : les entrees sont en clair sur le
// disque, et la cle de cache ignore les en-tetes (donc deux utilisateurs
// differents partageraient la meme entree).

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace tl::netcache {

struct Result {
    std::string body;
    bool ok = false;     // une reponse est disponible (fraiche ou perimee)
    bool fromCache = false;
    bool stale = false;  // servie depuis le cache APRES un echec reseau
    std::string error;   // renseigne si !ok
};

// Dossier du cache (<donnees>/cache/http).
std::filesystem::path dir();

// Lecture seule : nullopt si absent, ou plus vieux que `maxAge` secondes
// (maxAge <= 0 : n'importe quel age convient).
std::optional<std::string> peek(const std::string& url, long long maxAge);

// Ecrit une entree. Les reponses vides ne sont pas mises en cache : elles
// viennent presque toujours d'une erreur, et les servir plus tard ferait
// croire a un catalogue vide.
void put(const std::string& url, const std::string& body);

// Supprime une entree, puis tout le cache.
void drop(const std::string& url);
void clear();

// Taille totale sur disque, en octets.
long long size_bytes();

// Retire les entrees les plus anciennes jusqu'a repasser sous `maxBytes`.
void trim(long long maxBytes);

using Fetcher = std::function<std::optional<std::string>(const std::string& url)>;

// Le coeur : cache d'abord, reseau ensuite, cache perime en dernier recours.
//
// `maxAge` en secondes. `fetch` rend nullopt en cas d'echec reseau — c'est
// alors, et seulement alors, qu'une entree perimee est servie avec
// `stale = true`.
Result get(const std::string& url, long long maxAge, const Fetcher& fetch);

// Remet le cache a zero (tests).
void reset_for_tests();

} // namespace tl::netcache

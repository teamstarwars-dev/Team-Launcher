#pragma once

// Clés d'application pour l'API.
//
// Le jeton unique de la première version suffisait pour un script qu'on
// écrit soi-même. Il ne suffit plus dès qu'on **distribue** l'accès : on
// ne peut révoquer une application sans couper toutes les autres, on ne
// sait pas laquelle appelle, et le widget d'affichage reçoit le pouvoir
// de lancer une partie.
//
// D'où une clé par application, avec trois décisions :
//
//   - **Le secret n'est jamais conservé.** On range son empreinte SHA-1.
//     Lire `config.json` ne permet donc pas de s'authentifier, et le
//     secret n'est montré qu'une fois, à la création. Si on le perd, on
//     en régénère un — on ne peut pas le retrouver, et c'est voulu.
//   - **Des portées, pas un interrupteur.** `diag` analyse sans rien lire
//     de la machine ; `read` expose les instances ; `control` lance une
//     partie. Une application ne reçoit que ce dont elle a besoin.
//   - **Chaque appel laisse une trace** (date de dernier usage, compteur).
//     Sans cela, on ne sait pas quelle clé révoquer.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::apikeys {

// Portées, de la moins à la plus puissante.
//   diag    : analyser un log, une liste de mods. N'accède à RIEN de local.
//   read    : lire l'état du launcher et la liste des instances.
//   control : lancer une partie.
extern const char* const kScopeDiag;
extern const char* const kScopeRead;
extern const char* const kScopeControl;

bool is_known_scope(const std::string& s);

struct Key {
    std::string id;       // « ak_xxxxxxxx », visible, sert à révoquer
    std::string hash;     // SHA-1 du secret ; le secret n'est pas conservé
    std::string appName;  // à quoi elle sert, saisi à la création
    std::string contact;  // facultatif : qui l'a demandée
    std::vector<std::string> scopes;
    long long createdUnix = 0;
    long long lastUsedUnix = 0; // 0 = jamais appelée
    long long calls = 0;
    bool revoked = false;

    bool has(const std::string& scope) const;
};

// Clé créée : le secret n'est rendu QU'ICI.
struct Created {
    Key key;
    std::string secret; // à remettre au demandeur, impossible à relire après
};

Created create(const std::string& appName, const std::string& contact,
               const std::vector<std::string>& scopes);

std::vector<Key> list();
bool revoke(const std::string& id);
bool remove(const std::string& id);

// Recherche par secret présenté. nullopt si inconnu ou révoqué. Met à jour
// le compteur et la date de dernier usage (l'écriture est différée : un
// appel par seconde ne doit pas réécrire la configuration à chaque fois).
// `scope` vide = ne vérifie que l'existence.
struct Match {
    bool ok = false;
    std::string id;
    std::string appName;
    bool scopeDenied = false; // trouvée, mais sans la portée demandée
};
Match check(const std::string& secret, const std::string& scope);

// Purs, exposés pour les tests.
nlohmann::json to_json(const std::vector<Key>& keys);
std::vector<Key> from_json(const nlohmann::json& j);

// Écrit les compteurs en attente (arrêt de l'application).
void flush();

} // namespace tl::apikeys

#pragma once

// Plusieurs comptes Microsoft, avec bascule rapide.
//
// Le launcher ne savait garder qu'une session : se connecter avec un autre
// compte ecrasait la precedente, et revenir en arriere imposait une
// reconnexion complete. Sur une machine familiale ou partagee, c'est le
// scenario normal, pas l'exception.
//
// PRINCIPE : ce module ne manipule JAMAIS de secret en clair. L'etat d'une
// session tient dans deux fichiers deja chiffres par la plateforme
// (`session-cache.json` pour le jeton Minecraft, `msauth.json` pour le
// jeton de rafraichissement). Le magasin en conserve une copie TELLE
// QUELLE, et basculer revient a les remettre en place. Toute la chaine
// d'authentification existante continue donc de fonctionner sans la
// modifier, et aucun dechiffrement n'a lieu ici.
//
// Consequence a connaitre : ces blobs sont lies a la session Windows qui
// les a ecrits (DPAPI). Copier `accounts.json` sur une autre machine ou
// dans un autre profil ne donnera rien d'exploitable — c'est voulu.

#include <string>
#include <vector>

namespace tl::accounts {

struct Account {
    std::string uuid;
    std::string name;
    long long savedAt = 0; // epoch s. de la derniere mise a jour
    bool hasRefresh = false; // un jeton de rafraichissement est conserve
};

// Comptes connus, le plus recemment utilise en premier.
std::vector<Account> list();

// UUID du compte actif, "" si aucun.
std::string current_uuid();

// Enregistre la session courante dans le magasin (appele apres une
// connexion reussie). Sans effet s'il n'y a pas de session.
void remember_current();

// Bascule vers un compte connu : remet ses fichiers en place. Renvoie faux
// si l'UUID est inconnu ou si l'ecriture echoue. L'appelant doit ensuite
// recharger la session en memoire.
bool switch_to(const std::string& uuid);

// Retire un compte du magasin. Si c'est le compte actif, la session en
// cours est egalement effacee : garder une session active pour un compte
// qu'on vient d'oublier serait incoherent.
bool forget(const std::string& uuid);

// Chemin du magasin (<donnees>/accounts.json).
std::string store_path();

} // namespace tl::accounts

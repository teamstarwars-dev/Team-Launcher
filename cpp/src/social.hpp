#pragma once

// Social : amis, messages texte, blocage et signalement.
//
// DECISION DU 30/09/2026 : tout passe par Discord — amis, messages ET
// vocal. Consequence directe : le launcher n'heberge aucun serveur, ne
// conserve aucune donnee personnelle et n'a aucune moderation a assurer.
// Une seule identite, coherente avec le vocal deja tranche.
//
// Ce qu'il faut savoir avant de lire la suite : les amis et les messages
// prives de Discord ne sont PAS accessibles par le canal IPC local que le
// launcher utilise deja pour la Rich Presence. Celui-ci ne sait que
// declarer une activite. Il faut le **SDK social de Discord**, une
// bibliotheque native a recuperer sur le portail developpeur apres
// acceptation de leurs conditions, et liee a l'application.
//
// Ce fichier definit donc la couture, pas le transport. L'interface, le
// modele de donnees et les regles (blocage, signalement, vocal a venir) ne
// dependent pas du fournisseur et ne seront pas a refaire : brancher le
// SDK revient a implementer `Provider`.

#include <functional>
#include <string>
#include <vector>

namespace tl::social {

enum class Presence { Offline, Online, Playing };

struct Friend {
    std::string id;       // identifiant stable cote fournisseur
    std::string name;     // pseudo affiche
    Presence presence = Presence::Offline;
    std::string activity; // « Joue a ... », vide si rien
    bool blocked = false;
    int unread = 0;
};

struct Message {
    std::string id;
    std::string authorId;
    std::string text;
    long long unixTime = 0;
    bool mine = false;
    bool pending = false; // envoye, pas encore confirme
};

// Etat du fournisseur. `ready` faux = rien n'est joignable, et `detail`
// explique pourquoi en clair (a afficher tel quel a l'utilisateur).
struct Status {
    bool ready = false;
    bool voiceAvailable = false; // phase vocale, volontairement pas encore la
    std::string providerName;
    std::string detail;
};

Status status();

// Les listes sont vides tant que le fournisseur n'est pas pret. Aucune
// donnee fictive n'est jamais rendue : une fausse liste d'amis donnerait
// l'illusion d'une fonctionnalite qui n'existe pas.
std::vector<Friend> friends();
std::vector<Message> conversation(const std::string& friendId);

// Toutes ces actions renvoient faux et renseignent `errOut` tant que le
// fournisseur n'est pas pret.
bool add_friend(const std::string& pseudo, std::string* errOut);
bool send(const std::string& friendId, const std::string& text,
          std::string* errOut);
bool set_blocked(const std::string& friendId, bool blocked,
                 std::string* errOut);
bool report(const std::string& friendId, const std::string& reason,
            std::string* errOut);

// Demarre / arrete la liaison avec le fournisseur.
void start();
void stop();

} // namespace tl::social

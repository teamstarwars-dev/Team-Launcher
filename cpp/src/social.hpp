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
    // Vrai quand le fournisseur est operationnel mais qu il manque juste
    // la liaison du compte : l interface propose alors le bouton. Faux si
    // le SDK est absent, ou l integration desactivee — proposer de lier un
    // compte n y changerait rien.
    bool canLink = false;
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

// Derniere erreur d'envoi asynchrone (l'envoi repond apres coup) : la
// consommer affiche le probleme une fois, puis l'oublie.
std::string take_send_error();
bool set_blocked(const std::string& friendId, bool blocked,
                 std::string* errOut);
bool report(const std::string& friendId, const std::string& reason,
            std::string* errOut);

// --- Appels vocaux privés (1:1) -------------------------------------------
// Un appel = un lobby Discord a deux (secret aleatoire echange en DM) +
// StartCall, l'audio passant par les peripheriques par defaut via le SDK.
// La signalisation (invitation / acceptation / refus / fin) transite en
// metadonnees de DM, interceptees avant affichage : invisible dans le
// launcher, lisible en clair dans Discord.
struct CallInfo {
    bool idle = true;      // aucun appel
    bool outgoing = false; // on appelle, en attente du correspondant
    bool incoming = false; // on est appele, en attente de decrochage
    bool active = false;   // voix etablie
    std::string peerId;
    std::string peerName;
    bool muted = false;
    bool deaf = false;
    long long startedUnix = 0; // voix etablie a cette heure, 0 sinon
};
CallInfo call_info();
bool call_start(const std::string& friendId, std::string* errOut);
bool call_accept(std::string* errOut);
bool call_decline(std::string* errOut);
bool call_hangup(std::string* errOut);
bool call_set_muted(bool muted, std::string* errOut);
bool call_set_deaf(bool deaf, std::string* errOut);
// Dernier evenement d'appel (« Appel refusé. »...) : consomme une fois.
std::string take_call_notice();
// Lance la liaison du compte Discord. Le SDK ouvre lui-meme l'ecran
// d'autorisation dans l'application Discord : le launcher n'a ni serveur de
// redirection ni code a afficher, contrairement a l'authentification
// Microsoft. Sans effet si une liaison est deja en cours.
void begin_login();

// Une liaison est-elle en cours ? Sert a griser le bouton.
bool login_in_progress();

// Delie le compte : oublie le jeton et se deconnecte.
void logout();

// Demarre / arrete la liaison avec le fournisseur.
void start();

// A appeler une fois par frame : le SDK exige d etre pompe regulierement
// pour delivrer ses callbacks. Sans effet si le fournisseur est absent.
void pump();
void stop();

} // namespace tl::social

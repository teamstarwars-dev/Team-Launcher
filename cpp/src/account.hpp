#pragma once

// Compte Team Launcher (API maison : amis, messages, signaling vocal).
//
// Complementaire du module social (Discord) : ici l'annuaire, les fils et
// la signalisation d'appel vivent sur NOTRE serveur (URL configurable,
// jeton chiffre comme les autres secrets). Seul l'AUDIO d'un appel reste
// sur un lobby vocal Discord via social::voice_* — ce qui exige en plus
// un Discord lie et pret, sinon l'appel est refuse avec un message clair.
//
// Threading : un worker de fond fait le long-polling /api/events et les
// rafraichissements ; tout acces au cache est verrouille. pump() tourne
// sur le thread d'interface (timeouts d'appel).

#include <string>
#include <vector>

namespace tl::account {

struct Status {
    bool configured = false; // URL d'API renseignee
    bool loggedIn = false;   // jeton present (valide au dernier controle)
    std::string username;    // pseudo du compte
    std::string detail;      // message lisible quand indisponible
};

struct Friend {
    std::string name;
    bool online = false;
    int unread = 0;
    bool blocked = false; // bloque par moi (section dediee + deblocage)
};

struct Message {
    long long id = 0;
    bool mine = false;
    std::string sender;
    std::string text;
    long long unixTime = 0;
    bool pending = false;
};

struct CallInfo {
    bool idle = true;
    bool outgoing = false; // invitation envoyee, en attente
    bool incoming = false; // invitation recue, a decrocher
    bool active = false;   // voix etablie
    std::string peer;
    bool muted = false;
    bool deaf = false;
    bool denoise = false;
    long long startedUnix = 0;
};

Status status();
std::string take_notice(); // evenement ponctuel (erreur, refus...) : une fois

bool register_account(const std::string& username, const std::string& password,
                      std::string* errOut);
bool login(const std::string& username, const std::string& password,
           std::string* errOut);
void logout(); // invalide la session cote serveur + oublie le jeton

std::vector<Friend> friends();
std::vector<std::string> pending_in();
std::vector<std::string> pending_out();
bool friend_request(const std::string& username, std::string* errOut);
bool friend_accept(const std::string& username, std::string* errOut);
bool friend_decline(const std::string& username, std::string* errOut);
bool friend_remove(const std::string& username, std::string* errOut);
bool friend_block(const std::string& username, std::string* errOut);
bool friend_unblock(const std::string& username, std::string* errOut);

std::vector<Message> conversation(const std::string& peer);
bool send(const std::string& peer, const std::string& text,
          std::string* errOut);
std::string take_send_error();

CallInfo call_info();
bool call_start(const std::string& peer, std::string* errOut);
bool call_accept(std::string* errOut);
bool call_decline(std::string* errOut);
bool call_hangup(std::string* errOut);
bool call_set_muted(bool muted, std::string* errOut);
bool call_set_deaf(bool deaf, std::string* errOut);
bool call_set_denoise(bool denoise, std::string* errOut);
std::string take_call_notice();

void start(); // charge la session, demarre le worker si configure
void stop();  // arrete le worker (garde le jeton pour le prochain demarrage)
void pump();  // timeouts d'appel, sur le thread d'interface

} // namespace tl::account

#pragma once

// Phase 8 — API HTTP locale.
//
// À quoi elle sert : qu'un autre outil de la machine — un script, un
// Stream Deck, une barre de tâches, un bot — puisse demander l'état du
// launcher et lancer une instance, sans qu'on écrive une intégration par
// outil.
//
// Elle est **désactivée par défaut** et le reste tant qu'on ne lui donne
// pas un port. Quatre règles, toutes nécessaires :
//
//   1. **127.0.0.1 uniquement.** Jamais 0.0.0.0. Une API qui lance des
//      programmes et expose le nom du joueur n'a rien à faire sur le
//      réseau local, encore moins derrière un port ouvert par UPnP.
//   2. **Un jeton sur chaque requête** (`Authorization: Bearer …`), y
//      compris en lecture. Sur une machine partagée, « localhost » n'est
//      pas une frontière : tout autre programme de la session peut s'y
//      connecter.
//   3. **Rien n'est servi depuis l'état vivant.** Le serveur tourne sur
//      son propre fil ; lire `DataStore::settings` pendant que
//      l'interface le modifie serait un comportement indéfini. L'interface
//      publie un instantané JSON, le serveur sert cet instantané.
//   4. **Aucune action n'est exécutée par le fil du serveur.** Une demande
//      de lancement est déposée dans une file, que la boucle de
//      l'interface vide à la frame suivante.
//
// Points d'entrée :
//   GET  /api/status     état, version, instance sélectionnée
//   GET  /api/instances  liste des instances
//   POST /api/launch     corps {"id":"…"} — dépose une demande
// Tout le reste répond 404, et une requête sans jeton valable 401.

#include <string>
#include <vector>

namespace tl::localapi {

// Instantané publié par l'interface, servi tel quel.
void publish(const std::string& statusJson, const std::string& instancesJson);

// Demandes de lancement déposées par l'API, à consommer par l'interface.
std::vector<std::string> take_launch_requests();

// Démarre sur 127.0.0.1:port. Un port à 0, un jeton vide, ou une
// compilation sans serveur HTTP : ne démarre pas et renseigne errOut.
bool start(int port, const std::string& token, std::string* errOut = nullptr);
void stop();

bool running();
int port();
// Dernière erreur du serveur (port déjà pris...), à afficher telle quelle.
std::string last_error();

// Jeton aléatoire imprimable, pour la première activation.
std::string make_token();

} // namespace tl::localapi

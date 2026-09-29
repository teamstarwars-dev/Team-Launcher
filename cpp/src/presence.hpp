#pragma once

// Portage de PresenceService.cs — Rich Presence Discord.
//
// Le C# utilisait la bibliotheque DiscordRPC .NET. Ici on parle directement le
// protocole IPC de Discord : tube nomme \\.\pipe\discord-ipc-N (Windows) ou
// socket Unix $XDG_RUNTIME_DIR/discord-ipc-N (Linux), N de 0 a 9 ;
// trames « [opcode int32 LE][longueur int32 LE][charge utile JSON] ».
//   opcode 0 = HANDSHAKE, 1 = FRAME, 2 = CLOSE, 3 = PING, 4 = PONG
//
// Tout le dialogue vit sur un thread dedie : l'UI ne fait que deposer l'etat
// souhaite. Discord peut couper le lien a tout moment (redemarrage, mise a
// jour, fermeture) — un chien de garde reconnecte toutes les 20 s, comme le C#.

#include <string>

#include <nlohmann/json.hpp>

namespace tl::presence {

// Active ET identifiant d'application renseigne.
bool enabled();

// Demarre le thread de presence (sans effet si desactive). Idempotent.
void init();

// Presence « dans le launcher » : temps de jeu total.
void set_launcher();

// Presence en jeu : instance, chrono de session, ville RP si le serveur
// rejoint correspond a une ville de la team. server vide = solo.
void set_game(const nlohmann::json& inst, const std::string& server = {});

// Efface la presence, ferme le tube et joint le thread.
void shutdown();

// Relit les reglages : a appeler apres modification dans Parametres.
void reload();

// Etat pour l'UI : connecte a Discord ?
bool connected();

} // namespace tl::presence

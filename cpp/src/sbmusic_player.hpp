#pragma once

// Lecteur audio SB'Music (flux Ogg/Opus). Decodage via libopusfile, sortie via
// SDL audio. Compile seulement si le decodeur Opus est present (TL_HAVE_OPUS) ;
// sinon can_play() == false et le bouton de lecture reste inactif.
//
// Cut 1 : on telecharge le titre entier en memoire (quelques Mo) puis on le
// decode (op_open_memory). Simple et robuste ; le vrai streaming par plages
// pourra venir ensuite.

#include <string>

namespace tl::sbmusic {

// Le decodeur Opus est-il disponible (compile) ?
bool can_play();

// Joue un titre : telecharge le flux (authentifie) puis lance le decodage.
// La PARTIE TELECHARGEMENT bloque -> appeler depuis un worker. Rend false en
// cas d'echec ; `err` recoit un message le cas echeant.
bool play(const std::string& trackId, std::string* err = nullptr);

// Pause / reprise du titre courant.
void toggle_pause();

// Arrete et libere le titre courant.
void stop();

bool has_track();  // un titre est charge
bool is_playing(); // charge ET non en pause

double position_sec(); // position de lecture
double duration_sec(); // duree totale (0 si inconnue)
void seek(double sec);

void set_volume(float v); // 0..1
float volume();

// A appeler a la fermeture de l'application.
void shutdown();

} // namespace tl::sbmusic

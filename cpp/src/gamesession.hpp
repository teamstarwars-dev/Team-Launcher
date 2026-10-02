#pragma once

// Rattrapage d'une partie dont le launcher n'a pas vu la fin.
//
// Le cas évident est le réglage « quitter le launcher quand la partie
// démarre » : plus personne ne surveille le jeu, donc ni temps de jeu,
// ni date de dernière session. Mais le trou existait **avant** ce
// réglage, et pour tout le monde : fermer le launcher pendant qu'on joue,
// un plantage du launcher, une coupure de courant, un redémarrage de
// Windows — dans tous ces cas la partie n'était comptée nulle part.
//
// Le principe : avant de perdre de vue le jeu, on écrit un **marqueur de
// session** sur le disque. Au démarrage suivant, le launcher le relit et
// rattrape ce qu'il peut.
//
// POURQUOI PAS UN PROCESSUS DE SURVEILLANCE. On pourrait laisser un petit
// programme attendre la fin du jeu pour écrire la durée exacte. Mais ce
// serait exactement ce que l'utilisateur a voulu éviter en choisissant
// « quitter » : un processus qui traîne. Et deux programmes écrivant
// `config.json` en même temps, c'est une corruption qui attend son
// heure.
//
// COMMENT ON DEVINE L'HEURE DE FIN. Le jeu écrit sans arrêt pendant
// qu'on joue : `logs/latest.log` à chaque événement, les mondes à chaque
// sauvegarde automatique. La date de dernière modification de ces
// fichiers situe donc la fin de la partie à la minute près. C'est une
// estimation, et elle est annoncée comme telle dans le journal — mais
// une durée approchée vaut infiniment mieux qu'une durée perdue.

#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

namespace tl::gamesession {

struct Session {
    std::string instanceId;
    unsigned long pid = 0;
    long long startedUnix = 0;
};

// Chemin du marqueur (<données>/session.json).
std::filesystem::path marker_path();

// Écrit le marqueur. Appelé quand le launcher cesse de surveiller une
// partie en cours — au lancement si le réglage dit « quitter », et à
// l'arrêt du launcher si une partie tourne encore.
bool begin(const Session& s);

// Efface le marqueur : le launcher a vu la fin de la partie lui-même.
void clear();

// Le marqueur courant, s'il y en a un.
std::optional<Session> current();

struct Recovered {
    bool applied = false;      // du temps de jeu a été rattrapé
    bool stillRunning = false; // la partie tourne encore : on n'a rien fait
    std::string instanceId;
    std::string instanceName;
    long long seconds = 0;
};

// À appeler au démarrage. Si un marqueur traîne :
//   - la partie tourne encore  -> on ne touche à rien, on réessaiera.
//   - la partie est terminée   -> temps de jeu et date ajoutés, marqueur
//                                 effacé.
// Ne lance jamais. N'écrit la configuration que s'il y a quelque chose à
// écrire.
Recovered reconcile();

// Purs (testables) : estimation de la fin d'une partie à partir des
// fichiers que le jeu a écrits, et bornage de la durée.
long long last_activity_unix(const std::filesystem::path& gameDir);
// Borne la durée : jamais négative, et jamais plus que `maxSeconds`
// (une session de plus de 24 h est presque toujours un PC resté allumé,
// pas quelqu'un qui a joué).
long long clamp_duration(long long startedUnix, long long endUnix,
                         long long maxSeconds = 24 * 3600);

} // namespace tl::gamesession

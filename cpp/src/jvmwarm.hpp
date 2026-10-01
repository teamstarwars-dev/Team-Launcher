#pragma once

// Phase 8 — préchauffage avant lancement.
//
// CE QUE CE MODULE NE FAIT PAS, et ne peut pas faire : garder une machine
// virtuelle Java allumée en attendant qu'on clique sur Jouer. Un processus
// JVM déjà démarré ne peut pas être « redirigé » vers Minecraft — il
// faudrait qu'il ait été lancé dès le départ avec le bon classpath, les
// bons arguments et la bonne mémoire, c'est-à-dire qu'on connaisse déjà
// l'instance choisie. Prétendre le contraire serait un mensonge, et un
// processus Java inutile tournerait en fond sur la machine.
//
// Ce qu'on peut faire, et qui coûte réellement cher au lancement :
//
//   1. **Le balayage des Java installés.** `find_java` parcourt plusieurs
//      arborescences et exécute un `java -version` par candidat. Plusieurs
//      centaines de millisecondes, à chaque partie. On le fait d'avance,
//      pendant que l'utilisateur regarde ses instances, et le résultat est
//      mis en cache par `find_java` lui-même.
//
//   2. **Le cache disque du système.** Lire les premiers octets du jar du
//      client et des plus grosses bibliothèques amène leurs pages en
//      mémoire. Au lancement, la JVM les retrouve sans toucher le disque.
//      C'est l'essentiel du gain sur un disque mécanique, et c'est nul sur
//      un SSD déjà chaud — on ne promet donc rien de chiffré.
//
// Le travail se fait sur un fil de fond, à priorité basse d'intention : il
// est annulable à tout moment et ne retient jamais le lancement. Si
// l'utilisateur clique sur Jouer pendant le préchauffage, le lancement
// part sans l'attendre et retombe simplement sur le chemin normal.

#include <atomic>
#include <string>

#include <nlohmann/json.hpp>

namespace tl::jvmwarm {

struct State {
    bool warming = false;
    bool ready = false;
    std::string instanceId;  // instance préchauffée
    std::string javaPath;    // vide = aucun Java trouvé
    int javaMajor = 0;
    int filesTouched = 0;
    long long bytesTouched = 0;
    long long ms = 0;        // durée du préchauffage
    std::string detail;      // à afficher tel quel
};

// Demande le préchauffage de cette instance. Sans effet si c'est déjà
// celle qui est prête, ou si un préchauffage est en cours. Un changement
// d'instance annule le précédent : préchauffer celle qu'on vient de
// quitter ne sert à rien.
void request(const nlohmann::json& inst);

State state();

// Annule et joint le fil (arrêt de l'application).
void stop();

} // namespace tl::jvmwarm

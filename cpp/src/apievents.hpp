#pragma once

// Événements sortants — le launcher appelle, au lieu d'être interrogé.
//
// C'est le point qui change la nature du support. Aujourd'hui, un joueur
// qui plante doit penser à aller chercher son log, le coller quelque part
// et attendre une réponse. Avec un abonnement, le bot de la communauté
// reçoit le crash **et sa cause déjà analysée** à la seconde où il arrive,
// sans que le joueur ait rien fait.
//
// Et cela vaut même si personne n'écrit de client : recevoir une requête
// POST, c'est dix lignes dans n'importe quel outil.
//
// Trois règles de sûreté, toutes nécessaires :
//
//   - **HTTPS obligatoire**, sauf vers une adresse de bouclage. Le corps
//     contient le nom de l'instance, la version du jeu et un extrait de
//     journal ; le secret d'abonnement voyage dans un en-tête. En clair
//     sur le réseau, les deux seraient lisibles par n'importe qui.
//   - **L'hôte est ajouté à l'allowlist sortante** au moment où on
//     l'enregistre, jamais implicitement : le refus par défaut de la
//     couche HTTP reste la règle.
//   - **Jamais sur le fil de l'interface.** Une URL qui ne répond pas
//     bloquerait la fenêtre pendant le délai d'expiration, juste après un
//     crash — le pire moment.

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::events {

// Types d'événements. Chaînes stables : ce sont des valeurs d'API.
extern const char* const kCrash;       // partie terminée anormalement
extern const char* const kGameStart;   // partie lancée
extern const char* const kGameStop;    // partie terminée normalement

bool is_known_event(const std::string& t);

struct Hook {
    std::string id;
    std::string url;
    std::string secret; // envoyé en en-tête X-TeamLauncher-Secret, facultatif
    std::vector<std::string> events;
    bool enabled = true;
    long long lastUnix = 0; // dernière tentative
    bool lastOk = false;
    int failures = 0;      // échecs consécutifs
    std::string lastError; // à afficher tel quel
};

std::vector<Hook> list();

// Rejette une URL non HTTPS (hors bouclage) et renseigne errOut.
bool add(const std::string& url, const std::string& secret,
         const std::vector<std::string>& events, std::string* errOut = nullptr);
bool remove(const std::string& id);
void set_enabled(const std::string& id, bool on);

// Dépose un événement. Retourne immédiatement : l'envoi se fait sur un fil
// de fond. Sans abonné pour ce type, ne fait rien du tout.
void emit(const std::string& type, const nlohmann::json& payload);

// Envoie un événement d'essai à un abonnement, et attend la réponse. Le
// seul appel bloquant du module, réservé au bouton « Tester ».
bool test(const std::string& id, std::string* errOut = nullptr);

// Enregistre les hôtes auprès de la couche HTTP (au démarrage).
void start();
// Vide la file et joint le fil.
void stop();

// Pur (testable) : une URL est-elle acceptable comme destination ?
bool url_acceptable(const std::string& url, std::string* whyNot = nullptr);

} // namespace tl::events

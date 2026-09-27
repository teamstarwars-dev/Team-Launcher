#pragma once

// Portage d'AdminService.cs — telemetrie d'administration vers adminServerUrl
// (battement periodique, evenements, rapports d'erreur).
//
// Desactivee par defaut (`adminTelemetryEnabled` = false) : rien ne part tant
// que l'utilisateur ne l'active pas, exactement comme le C#.
//
// DIVERGENCE vs C# : le C# lancait trois processus `wmic` a CHAQUE battement
// pour la RAM, le CPU et le GPU. `wmic` est deprecie et absent de Windows 11
// 24H2 et suivants — il y renvoyait donc des champs vides, en payant quand
// meme trois creations de processus. Ce port lit les memes informations par
// API native (GlobalMemoryStatusEx, registre, EnumDisplayDevices) : aucun
// sous-processus, resultat immediat, et les champs sont reellement remplis.

#include <string>

#include <nlohmann/json.hpp>

namespace tl::admin {

// Active ET URL de serveur renseignee.
bool enabled();

// Identifiant d'installation anonyme, genere puis conserve au premier appel
// (le C# le posait par defaut dans le modele).
std::string installation_id();

// Demarre le battement (5 min) et signale « launcher_start ». Idempotent,
// sans effet si desactivee.
void start();

// Arrete le thread de battement.
void stop();

// Envois ponctuels (non bloquants, silencieux en cas d'echec).
void send_heartbeat();
void send_event(const std::string& eventType, const nlohmann::json& data = nullptr);
void report_error(const std::string& source, const std::string& message,
                  const std::string& stackTrace = {},
                  const std::string& mcVersion = {},
                  const std::string& loader = {});

// --- Informations machine (exposees pour les tests) ---
struct Machine {
    std::string hostname;
    std::string osVersion;
    std::string cpuName;
    std::string gpuName;
    long long ramMb = 0;
};
Machine machine_info();

// Corps du battement, sans envoi : permet de verifier la forme du JSON.
nlohmann::json heartbeat_payload();

} // namespace tl::admin

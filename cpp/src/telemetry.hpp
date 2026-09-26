#pragma once

// Portage de TelemetryService.cs — rapports vers un webhook Discord.
//
// Rien n'est envoye tant que l'utilisateur n'a pas colle un webhook dans
// Parametres > Integrations (`discordTelemetryWebhook` vide par defaut), meme
// si `telemetryEnabled` vaut true — comportement identique au C#.
//
// Les envois passent par un worker unique : `stop()` le joint au shutdown.

#include <string>

#include <nlohmann/json.hpp>

namespace tl::telemetry {

// Active ET webhook renseigne.
bool enabled();

// inst = objet instance du config (Name/Id/Loader/McVersion/Launches).
void report_crash(const nlohmann::json& inst, int exitCode,
                  const std::string& gameLogTail = {});
void report_launch(const nlohmann::json& inst);
void report_instance_deleted(const nlohmann::json& inst);
void report_startup();

// Joint le worker d'envoi (shutdown de l'app).
void stop();

} // namespace tl::telemetry

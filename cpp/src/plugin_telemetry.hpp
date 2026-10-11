#pragma once
// V8 — télémétrie ANONYME des plugins, désactivée par défaut.
//
// Ce qui part, et rien d'autre : « tel plugin signé, en telle version, a
// été arrêté N fois pour telle raison », plus la version du launcher et le
// système (windows/linux). Aucun identifiant, aucun nom d'instance, aucun
// texte libre : les raisons sont une liste fermée. Seuls les plugins
// SIGNÉS sont comptés — un plugin en mode développeur peut porter un nom
// de dossier personnel, qui ne doit jamais quitter le PC.
//
// Le serveur n'en garde que des totaux par jour (marketplace/app/models.py,
// TelemetryDaily), sans adresse.

#include <cstddef>
#include <map>
#include <string>
#include <tuple>

namespace tl::plugtele {

// Raison stable à partir d'un événement du plugin host, ou "" si
// l'événement ne se compte pas (démarrage normal, par exemple).
std::string classify(const std::string& kind, const std::string& detail);

// (plugin, version, raison) -> nombre
using Counts = std::map<std::tuple<std::string, std::string, std::string>, long>;

// Compte un événement (sûr entre fils). Plafonné : au-delà de 200 clés
// différentes, les nouvelles sont ignorées jusqu'au prochain envoi.
void record(const std::string& plugin, const std::string& version, const std::string& kind);

// Retire et rend tout ce qui est en attente. Si l'envoi échoue, restore()
// le remet (fusionné avec ce qui a été compté entre-temps).
Counts take();
void restore(const Counts& c);
std::size_t pending();

// Document JSON envoyé au serveur (pur, testable).
std::string payload(const Counts& c, const std::string& launcherVersion, const std::string& os);

void reset_for_tests();

} // namespace tl::plugtele

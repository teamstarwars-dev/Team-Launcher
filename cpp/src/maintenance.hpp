#pragma once

// Portage de HealthService.cs, CleanupService (ServerPing.cs) et de la partie
// portable d'UpdateService.cs / UpdateChecker.cs.

#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace tl::health {

struct Check {
    std::string name;
    bool ok = false;
    std::string detail;
};

// Diagnostic complet (bloquant : une requete reseau de 6 s au plus).
std::vector<Check> run_all(const std::atomic<bool>* cancel = nullptr);

} // namespace tl::health

namespace tl::cleanup {

struct Result {
    int files = 0;
    double mb = 0.0;
};

// Installeurs Forge/NeoForge et archives JRE du dossier runtime, plus les .zip
// de plus de 30 jours du dossier de donnees.
Result run();

} // namespace tl::cleanup

namespace tl::updates {

// Version compilee (TL_VERSION_STRING).
const char* current_version();

struct Info {
    std::string version; // tag sans le « v » initial
    std::string notes;   // corps de la release
    std::string url;     // page de la release
};

// Derniere release GitHub. nullopt = deja a jour, ou verification impossible
// (errOut renseigne dans ce second cas).
std::optional<Info> check(std::string* errOut = nullptr);

// Compare deux versions « a.b.c[.d] » : <0, 0, >0.
int compare_versions(const std::string& a, const std::string& b);

} // namespace tl::updates

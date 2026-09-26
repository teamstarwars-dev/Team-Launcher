#pragma once

// Portage de GameLauncher.PlayCore (C#) : orchestration complete
// session -> version -> install -> Java -> args -> demarrage du jeu.
// Sans Instances C# pour l'instant (gameDir explicite), sans auth Microsoft
// (offline uniquement — reste documente), sans backup/telemetrie/crash-analyzer.

#include <atomic>
#include <functional>
#include <string>

#include "game_launcher.hpp"

namespace tl {

struct LaunchRequest {
    std::string version;   // "", "latest", "?" ou "" -> derniere release Mojang
    std::string loader = "Vanilla";
    std::string gameDir;   // vide -> instancesRoot()/default (cree si absent)
    std::string instanceId; // Id de l'instance (sauvegarde des mondes) ; vide = aucune
    std::string jvmArgs;   // arguments JVM supplementaires (inst.JvmArgs)
    int ramGb = 0;         // 0 = DataStore::settings.maxRamGb
    std::string joinServer; // vide = aucun --server/--port
};

struct LaunchUi {
    std::function<void(const char* stage, int done, int total)> progress;
    std::function<void(const char* status)> status;
    std::function<void(const char* line)> log;
};

struct LaunchResult {
    GameProcess proc{};
    bool started = false;   // processus Java demarre (fermer via close_game)
    bool cancelled = false;
    std::string error;      // si !started && !cancelled
};

// Bloquant (reseau + installation + lancement). cancel -> cancelled=true.
LaunchResult launch_flow(const LaunchRequest& req, const LaunchUi& ui,
                         std::atomic<bool>& cancel);

} // namespace tl

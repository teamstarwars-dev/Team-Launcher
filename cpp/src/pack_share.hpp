#pragma once

// Portage de PackShareService.cs — partage d'une instance entre membres de la
// team, sous forme d'un descriptif JSON que l'on colle dans Discord.
//
// Ce qui est porte : l'export (empreintes SHA1 des mods et shaders, resolution
// groupee sur Modrinth) et l'import (retelechargement depuis les URL du
// descriptif). C'est le seul chemin fonctionnel du C#.
//
// Ce qui n'est PAS porte, volontairement : le « code court » type ABCD-EFGH de
// InstancesPage.GenerateShareCode. Il est derive des SHA1 mais rien ne sait le
// resoudre — il n'existe ni serveur ni registre, et ImportAsync n'accepte que
// le JSON complet. Le proposer donnerait un bouton qui ne peut pas marcher.

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::share {

inline constexpr const char* kFormatId = "teamlauncher-pack-v2";

using Progress = std::function<void(const std::string&)>;

struct Stats {
    int mods = 0;
    int shaders = 0;
    int recognizedMods = 0;   // resolus sur Modrinth (donc retelechargeables)
    int recognizedShaders = 0;
};

// Analyse l'instance et construit le descriptif partageable.
// `pack` est null en cas d'echec, `error` renseigne.
struct ExportResult {
    nlohmann::json pack = nullptr;
    Stats stats;
    bool cancelled = false;
    std::string error;
};
ExportResult export_pack(const nlohmann::json& inst, const Progress& progress,
                         std::atomic<bool>& cancel);

// JSON indente, pret a coller.
std::string serialize(const nlohmann::json& pack);

struct ImportResult {
    nlohmann::json instance = nullptr; // a ajouter au config par l'appelant
    int downloaded = 0;
    int failed = 0;
    bool cancelled = false;
    std::string error;
};

// Recree une instance depuis un descriptif colle. N'ecrit pas dans le config.
ImportResult import_pack(const std::string& json, const Progress& progress,
                         std::atomic<bool>& cancel);

// Le texte ressemble-t-il a un descriptif de pack ? (test avant import)
bool looks_like_pack(const std::string& text);

} // namespace tl::share

#pragma once

// Portage de CrashAnalyzer.cs — traduit une erreur technique de Minecraft en
// explication claire avec la marche a suivre.

#include <filesystem>
#include <optional>
#include <string>

namespace tl::crash {

// Premiere regle qui correspond, ou nullopt si rien de connu.
std::optional<std::string> analyze(const std::string& logText);

// Analyse les journaux d'une instance : crash-reports/ recent (< 5 min) puis
// les 300 dernieres lignes de game-log.txt.
std::optional<std::string> analyze_instance(const std::filesystem::path& gameDir);

// n dernieres lignes d'un fichier texte (telemetrie, aperçu UI). "" si absent.
std::string tail_lines(const std::filesystem::path& file, int n);

} // namespace tl::crash

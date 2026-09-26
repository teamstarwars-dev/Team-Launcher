#pragma once

// Portage de GameLauncher.cs — cœur de lancement (sans UI).

#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl {

struct McSession {
    std::string name;
    std::string uuid;
    std::string accessToken;
};

// UUID offline : MD5("OfflinePlayer:"+name)[0..16] au format Guid .NET "N"
// (endianness .NET : 4+2+2 premiers octets en little-endian).
McSession offline_session(const std::string& name);

// Recherche du meilleur javaw.exe >= requiredMajor (cache inclus).
std::optional<std::string> find_java(int requiredMajor = 8);
int detect_java_major(const std::string& javawPath);

// Telecharge un JRE Adoptium dans runtime/jre-<major> (marqueur .done).
std::optional<std::string> download_java(int major,
                                         const std::function<void(const char*)>& status,
                                         std::atomic<bool>& cancel);

// RAM disponible/totale en Mo (GlobalMemoryStatusEx), -1 si erreur.
long long available_ram_mb();
long long total_ram_mb();

// Construction des lignes de commande (fidèle au C#).
std::vector<std::string> build_jvm_args(const std::string& classpath,
                                        const std::string& natives,
                                        bool isForge,
                                        const std::vector<std::string>* officialJvm,
                                        int ramGb,
                                        const std::string& extraJvmArgs);

std::vector<std::string> build_game_args(const std::string& version,
                                         const McSession& s,
                                         const std::string& assetsIndex,
                                         const std::string* legacyArgs,
                                         bool hasModernArgs,
                                         const std::string* joinServer);

// Journal launcher.log (mememo LOCALAPPDATA\TeamLauncher\launcher.log comme le C#).
void log_line(const std::string& text);

// Resultat d'un lancement.
struct GameProcess {
    void* hProcess = nullptr; // HANDLE, fermer par close_game
    void* outRead = nullptr;  // pipe stdout du jeu
    void* errRead = nullptr;  // pipe stderr du jeu
    unsigned long pid = 0;
};

// Lit la derniere version release depuis le manifeste Mojang.
std::optional<std::string> latest_release();

struct LaunchInput {
    std::string javaExe;
    std::string gameDir;
    std::string mainClass;
    std::vector<std::string> args; // jvm + mainClass + game args
};

// Demarre le processus Java (fenetre cachee, pipes sortants).
std::optional<GameProcess> start_game(const LaunchInput& in);
void close_game(GameProcess& g);

// Copie stdout+stderr du jeu vers gameLog (append), tant que le processus vit.
void start_game_log_writer(GameProcess& g, const std::filesystem::path& gameLog);

// Attend la fin du jeu. Retourne le code de sortie (-1 si erreur).
int wait_game(GameProcess& g);

} // namespace tl

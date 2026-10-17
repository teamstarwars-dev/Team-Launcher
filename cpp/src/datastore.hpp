#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "obf.hpp" // adminServerUrl obfusquee (S3, barriere `strings`)

namespace tl {

struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const noexcept;
};

// file_time_type -> secondes Unix. L'epoch de file_time_type n'est PAS Unix
// (1601 sous MSVC, autre sous libstdc++ — le compte est même négatif) :
// clock_cast (C++20) fait la conversion correcte sur les deux plateformes.
// Sans ça, tri/comparaisons par date sont faux sous Linux.
inline std::int64_t file_time_to_unix(std::filesystem::file_time_type t) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::clock_cast<std::chrono::system_clock>(t).time_since_epoch())
        .count();
}

struct AppSettings {
    std::string playerName = "Joueur";
    std::string accountMode;
    std::string javaPath;
    int maxRamGb = 4;
    std::string azureClientId;
    std::string instancesDir;
    std::string bgColor;
    std::string cardColor;
    std::string accentColor;
    std::string backgroundImagePath;
    bool fpsCounterEnabled = false;
    bool discordEnabled = false;
    std::string discordAppId;
    std::string updateUrl;
    std::string newsUrl;
    std::string language = "fr";
    std::string curseForgeApiKey;
    bool onboardingDone = false;
    // Modeles non encore portes : conservees telles quelles (compat config C#)
    nlohmann::json instances = nlohmann::json::array();
    nlohmann::json servers = nlohmann::json::array();
    std::vector<std::string> favoriteServers;
    nlohmann::json cities = nlohmann::json::array();
    nlohmann::json hostedServers = nlohmann::json::array();
    nlohmann::json pteroHosts = nlohmann::json::array(); // hotes Pterodactyl (server_host)
    bool autoShortcut = false;
    std::string vpsUrl;
    std::string vpsApiKey;
    bool telemetryEnabled = true;
    std::string discordTelemetryWebhook;
    std::string installationId;
    bool adminTelemetryEnabled = false;
    // Serveur d'admin : obfusque (S3) pour ne pas le laisser en clair dans
    // .rdata (il est de toute facon chiffre DPAPI dans config.json).
    // M4 : https obligatoire (admin::post refuse le clair hors boucle locale).
    // Un serveur http-only sur cette IP n'enverra donc plus rien tant qu'il
    // n'expose pas un endpoint HTTPS valide.
    std::string adminServerUrl = TL_OBF("https://51.255.207.183:3000");
    // Ce que fait le launcher quand la partie demarre : "nothing",
    // "minimize" ou "quit". Remplace minimizeOnLaunch, conserve juste
    // en dessous parce que la v5 Avalonia partage ce fichier de
    // configuration et ne connait que le booleen.
    std::string onGameLaunch = "minimize";
    bool minimizeOnLaunch = true;
    // ---- Phase A : réglages avancés (extensibles par les phases suivantes) --
    std::string logLevel = "Info"; // Trace/Debug/Info/Warn/Error/Fatal/Off
    std::string theme = "classic"; // classic/light
    std::string uiFont = "auto";   // « auto » ou chemin d une police systeme
    bool sidebarCompact = false;   // barre laterale en icones seules
    bool colorblind = false;       // accents à fort contraste (Okabe-Ito)
    double fontScale = 1.0;        // 0.8..1.6 via FontGlobalScale
    std::string contentPath;       // racine contenu (vide = DataStore::dir())
    int updateFreqHours = 24;      // vérification maj auto (phase G)
    std::string updateChannel = "stable"; // stable/beta
    int maxDownloads = 4;          // 1..20 (phase B)
    int backupSpaceMb = 0;         // quota sauvegardes en Mo, 0 = illimité
    int backupAutoHours = 0;       // sauvegarde auto toutes les N h, 0 = off
    int backupKeep = 10;           // nombre d archives conservees
    int analyseThreads = 4;        // 1..12 (analyse mods, phase D)
    // minimize/quit. Defaut « quit » : sans icone de zone de notification,
    // une fenetre qui ne se ferme pas quand on clique sur la croix passe
    // pour une panne. C'est un choix a faire, pas a subir.
    std::string closeBehavior = "quit";
    bool launchAtSystemStart = false;       // (phase G)
    std::string startupGame = "last";       // last/<id> (phase G)
    std::string lastGameId;
    std::string dateFormat = "dd/MM/yyyy"; // (phase G)
    // Version affichee au dernier demarrage. Sert a reconnaitre qu'une mise
    // a jour vient d'etre appliquee et a ne proposer les notes de version
    // qu'une fois. Vide = premiere execution : rien a montrer.
    std::string lastRunVersion;
    // Heure Unix de la derniere verification de mise a jour reussie ou
    // tentee. Sert au rythme automatique (UpdateCheckHours) ; 0 = jamais.
    long long lastUpdateCheckUnix = 0;
    // Salon d'entraide. Vide par defaut : mieux vaut un bouton desactive
    // qu'un lien d'invitation invente, qui ne menerait nulle part.
    std::string helpDiscordUrl;
    // Prechauffage avant lancement (phase 8) : reperage de Java et mise en
    // cache disque pendant qu'on choisit son instance. Lit des fichiers et
    // lance un `java -version` par candidat, d'ou le reglage — rien ne
    // doit s'activer en fond sans qu'on puisse l'arreter.
    bool jvmPreload = true;
    // Compte Team Launcher (API maison : amis, messages, signaling vocal).
    // URL de base vide = fonction desactivee. Le jeton est chiffre comme
    // les autres secrets (DPAPI / trousseau).
    std::string tlApiUrl;
    std::string tlApiToken;
    std::string tlApiUser;
    // API HTTP locale (phase 8) : 0 = desactivee. Jamais exposee hors de
    // 127.0.0.1, et toujours protegee par un jeton.
    int localApiPort = 0;
    std::string localApiToken;
    // Page Musique (SB'Music, OAuth 2.0 PKCE). Jetons chiffres comme les
    // autres secrets ; expiry = instant (unix) ou l'access token perime.
    std::string sbMusicAccessToken;
    std::string sbMusicRefreshToken;
    long long sbMusicTokenExpiry = 0;
    // Plugins : seuls ceux listes ici s'executent. Un plugin lance des
    // programmes ; il ne peut donc jamais etre actif par defaut du seul
    // fait d'avoir ete depose dans le dossier.
    std::vector<std::string> enabledPlugins;
    // V8 : autorise les plugins NON signés, pour qu'un développeur puisse
    // tester le sien avant de le soumettre. Désactivé par défaut. N'ouvre
    // jamais la porte à un plugin révoqué ou à signature fausse.
    bool pluginDevMode = false;
    // V8 : adresse du serveur de la marketplace (catalogue, téléchargements,
    // liste de révocation). HTTPS obligatoire, sauf 127.0.0.1/localhost
    // pour tester un serveur local. Vide = marketplace désactivée.
    std::string marketplaceUrl;
    // Dernière liste de révocation récupérée avec succès (Unix).
    long long lastRevocationSyncUnix = 0;
    // V8 (M3) : plancher anti-retour arrière des listes de révocation, et
    // sauvegarde de la dernière liste signée HORS du fichier revocations.json
    // (supprimable). Supprimer ce seul fichier ne désarme plus le
    // kill-switch ni les révocations, et une ancienne liste ne peut plus
    // être rejouée.
    long long revocationFloorSeq = 0;
    std::string revocationDoc;
    // V8 : télémétrie anonyme des plugins signés (plugin_telemetry.hpp).
    // DÉSACTIVÉE par défaut : c'est l'utilisateur qui coche.
    bool pluginTelemetry = false;
    // Clés d'application de l'API (apikeys.hpp). On n'y range que des
    // EMPREINTES de secrets, jamais les secrets : lire ce fichier ne doit
    // pas permettre de s'authentifier.
    nlohmann::json apiKeys = nlohmann::json::array();
    // Abonnements aux événements sortants (apievents.hpp).
    nlohmann::json eventHooks = nlohmann::json::array();
};

// Identifiant d'instance : 32 hexa (Guid .NET « N »), BCryptGenRandom / getrandom().
std::string new_guid();

// InstanceInfo C# (champs minimaux — les autres prennent les defauts C#).
nlohmann::json make_instance(const std::string& name, const std::string& loader,
                             const std::string& mcVersion);

// Portage de DataStore.cs (C#) : config.json partage avec la v5 Avalonia.
class DataStore {
public:
    static bool isPortable;

    // TL_DATA_DIR (test) > --portable (exe/data) > ContentPath (réglage,
    // le config migre au prochain enregistrement) > %LOCALAPPDATA%\TeamLauncher
    // (Windows) ou $XDG_DATA_HOME/TeamLauncher sinon ~/.local/share/TeamLauncher (Linux)
    static std::filesystem::path dir();
    static std::filesystem::path instancesRoot();
    static std::filesystem::path skinsDir();
    static std::filesystem::path imagesDir();
    static std::filesystem::path configPath();

    static void load();
    static void save();    // ecriture deboncee 500 ms
    static void saveNow(); // ecriture immediate (fin d'app)
    static void shutdown(); // arrete le worker debonce

    // Révocations (M3) : écrit revocationDoc + revocationFloorSeq PUIS persiste,
    // le tout sous le même verrou que la sérialisation. install_revocations
    // tourne sur un fil de rafraîchissement : passer par ici évite une course
    // de données entre l'écriture de ces champs et doSave() (qui lit settings).
    static void setRevocationState(const std::string& doc, long long floorSeq);

    static AppSettings settings;

private:
    static void loadDefaults();
    static void applyDefaults(bool applyBooleans);
    static void doSave();

    static std::map<std::string, std::string, CaseInsensitiveLess>& defaults();
};

} // namespace tl

#include "lang.hpp"

#include "datastore.hpp"

#include <cstring>
#include <string_view>
#include <unordered_map>

namespace tl::lang {

namespace {

// Dictionnaire fr -> en. Reprend `Lang.FrToEn` (MsAuth/MiscPages/pages C#) et
// y ajoute les libelles propres au portage C++ (sidebar sans emoji, modale
// d'auth, pages Serveurs/Skins/Bedrock reecrites en ImGui).
// Les cles sont les chaines FRANCAISES telles qu'ecrites dans les ui_*.cpp.
struct Entry {
    const char* fr;
    const char* en;
};

const Entry kTable[] = {
    // ---------------- navigation / chrome ----------------
    {"Accueil", "Home"},
    {"Instances", "Instances"},
    {"Jouer", "Play"},
    {"Serveurs", "Servers"},
    {"Skins", "Skins"},
    {"Actualités", "News"},
    {"Bedrock", "Bedrock"},
    {"Compte", "Account"},
    {"Paramètres", "Settings"},

    // ---------------- boutons courants ----------------
    {"Enregistrer", "Save"},
    {"Appliquer", "Apply"},
    {"Annuler", "Cancel"},
    {"Fermer", "Close"},
    {"Créer", "Create"},
    {"Modifier", "Edit"},
    {"Editer", "Edit"},
    {"Supprimer", "Delete"},
    {"Dupliquer", "Duplicate"},
    {"Ouvrir", "Open"},
    {"Importer", "Import"},
    {"Exporter .zip", "Export .zip"},
    {"Actualiser", "Refresh"},
    {"Valider", "Apply"},
    {"Parcourir...", "Browse..."},
    {"Réessayer", "Retry"},
    {"Gérer", "Manage"},
    {"Détails", "Details"},
    {"Démarrer", "Start"},
    {"Rejoindre", "Join"},
    {"Chargement…", "Loading…"},
    {"Nom", "Name"},
    {"Adresse", "Address"},
    {"Description", "Description"},
    {"Version", "Version"},
    {"Loader", "Loader"},
    {"Mods", "Mods"},
    {"Mondes", "Worlds"},
    {"Journaux", "Logs"},
    {"Screenshots", "Screenshots"},
    {"Favoris", "Favorites"},
    {"En ligne", "Online"},
    {"Hors ligne", "Offline"},
    {"Déconnecté", "Disconnected"},
    {"Se déconnecter", "Sign out"},

    // ---------------- page Accueil ----------------
    {"Bonjour", "Good morning"},
    {"Bonsoir", "Good evening"},
    {"Joueur", "Player"},
    {"A quoi voulez-vous jouer aujourd'hui ?", "What do you want to play today?"},
    {"REPRENDRE", "RESUME"},
    {"Vos instances", "Your instances"},
    {"Tout gérer >", "Manage all >"},
    {"Dernière session le ", "Last session on "},
    {"Jamais lancée", "Never launched"},
    {"Aucune instance pour le moment", "No instances yet"},
    {"Aucune instance pour le moment.", "No instances yet."},
    {"Créez une instance pour choisir une version de Minecraft et un chargeur de "
     "mods.\nLes fichiers communs sont partagés entre toutes vos instances, ce "
     "qui évite de télécharger plusieurs fois les mêmes gigaoctets.",
     "Create an instance to pick a Minecraft version and a mod loader.\nShared "
     "files are reused across all your instances, so you never download the same "
     "gigabytes twice."},
    {"+ Créer une instance", "+ Create an instance"},
    {"instance(s)", "instance(s)"},
    {"Lancements", "Launches"},

    // ---------------- page Instances ----------------
    {"+ Créer", "+ Create"},
    {"Importer (.zip)", "Import (.zip)"},
    {"Importer un dossier", "Import a folder"},
    {"Importer partagé (presse-papiers)", "Import shared (clipboard)"},
    {"Partager (copier le pack)", "Share (copy pack)"},
    {"Créer une instance", "Create instance"},
    {"Le nom est requis.", "A name is required."},
    {"Rechercher…", "Search…"},
    {"Rechercher une instance", "Search for an instance"},
    {"Temps de jeu", "Playtime"},
    {"Récemment jouée", "Recently played"},
    {"Petit", "Small"},
    {"Grand", "Large"},
    {"Éditer l'instance", "Edit instance"},
    {"Supprimer l'instance", "Delete instance"},
    {"Rien ne correspond à votre recherche.", "Nothing matches your search."},
    {"Aucune instance", "No instance"},
    {"Fenêtre Détails : portage en cours", "Details window: port in progress"},
    {"Erreur d'import", "Import error"},
    {"Erreur d'import : archive illisible.", "Import error: unreadable archive."},
    {"Import terminé : instance importée.", "Import complete: instance imported."},
    {"Erreur d'export.", "Export failed."},
    {"Exporter l'instance", "Export instance"},
    {"Essayez un autre terme, ou changez le tri.",
     "Try another term, or change the sort order."},

    // ---------------- page Jouer ----------------
    {"Lancement…", "Launching…"},
    {"En jeu…", "In game…"},
    {"Lancement annulé.", "Launch cancelled."},
    {"Minecraft en cours d'exécution...", "Minecraft is running..."},
    {"Échec du lancement.", "Launch failed."},
    {"Pret.", "Ready."},
    {"Journal", "Log"},
    {"Prêt.", "Ready."},
    {"Aucune instance - créez-en une depuis la page Instances.",
     "No instance - create one from the Instances page."},
    {"Jeu fermé (code de sortie ", "Game closed (exit code "},
    {"Minecraft s'est arrêté anormalement", "Minecraft stopped unexpectedly"},
    {"Minecraft s'est arrêté", "Minecraft stopped"},

    // ---------------- page Serveurs ----------------
    {"Mes serveurs", "My servers"},
    {"Villes de la team", "Team cities"},
    {"Nouveau serveur", "New server"},
    {"Supprimer ce serveur ?", "Delete this server?"},
    {"Ping en cours...", "Pinging..."},
    {"Impossible de joindre", "Unreachable"},
    {"Adresse du serveur (ex: mc.example.com)", "Server address (e.g. mc.example.com)"},
    {"Aucun serveur favori.\nAjoutez une adresse ci-dessus.",
     "No favorite server.\nAdd an address above."},
    {"Aucune ville.\nAjoutez-en une ci-dessus.", "No city.\nAdd one above."},
    {"Clique sur « Nouveau serveur » pour en créer un.",
     "Click \"New server\" to create one."},
    {"Propriétaire (optionnel)", "Owner (optional)"},
    {"Non installé", "Not installed"},
    {"Nouveau serveur hébergé", "New hosted server"},
    {"Aucun serveur hébergé.\nClique sur « Nouveau serveur » pour en créer un.",
     "No hosted server.\nClick \"New server\" to create one."},
    {"Clique sur un serveur pour ouvrir le panneau de gestion (console, joueurs, "
     "map, réglages).",
     "Click a server to open the management panel (console, players, map, "
     "settings)."},
    {"Partager", "Share"},
    {"Nom :", "Name:"},
    {"Version Minecraft :", "Minecraft version:"},
    {"Chargeur :", "Loader:"},
    {"Port :", "Port:"},
    {"RAM max (Go) :", "Max RAM (GB):"},
    {"+ Ajouter", "+ Add"},
    {"- Supprimer", "- Remove"},

    // ---------------- page Skins ----------------
    {"Ma bibliothèque", "My library"},
    {"Tous", "All"},
    {"Officiels", "Official"},
    {"Catalogue en ligne", "Online catalog"},
    {"Récupérer le mien", "Get mine"},
    {"Appliquer sur mon compte", "Apply to my account"},
    {"Favori", "Favorite"},
    {"Recadrer", "Crop"},
    {"Glissez pour tourner - molette pour zoomer",
     "Drag to rotate - scroll to zoom"},
    {"Chargement du catalogue en ligne...", "Loading online catalog..."},
    {"Choisissez un skin dans la bibliothèque, ou importez-en un.",
     "Pick a skin from the library, or import one."},
    {"Aucun favori. Sélectionne un skin puis clique « Favori ».",
     "No favorite. Select a skin then click \"Favorite\"."},
    {"Aucun skin officiel. Clique sur « Récupérer le mien ».",
     "No official skin. Click \"Get mine\"."},
    {"Aucun skin choisi", "No skin selected"},
    {"Retirer favori", "Remove favorite"},
    {"Exporter", "Export"},
    {"Dossier", "Folder"},
    {"Depuis un fichier", "From a file"},
    {"Depuis un pseudo", "From a username"},
    {"Depuis une adresse", "From a URL"},
    {"Ouvrir le dossier", "Open the folder"},
    {"Pseudo Minecraft :", "Minecraft username:"},
    {"URL du fichier .png :", ".png file URL:"},
    {"Rechercher un skin ou un pseudo...", "Search for a skin or username..."},
    {"Appliquer à toutes les instances", "Apply to all instances"},
    {"COMPTE", "ACCOUNT"},
    {"OK", "OK"},
    {"Catalogue vide. Clique sur « Catalogue en ligne » ou cherche un pseudo.",
     "Empty catalog. Click \"Online catalog\" or search for a username."},
    {"Votre bibliothèque est vide. Importez un skin depuis un fichier, depuis le "
     "pseudo d'un joueur, ou depuis une adresse.",
     "Your library is empty. Import a skin from a file, from a player's username, "
     "or from a URL."},
    // statuts de la page Skins (traduits a l'affichage — le worker ecrit en fr)
    {"Chargement du catalogue NameMC...", "Loading the NameMC catalog..."},
    {"Téléchargement...", "Downloading..."},
    {"Application...", "Applying..."},
    {"Skin exporté.", "Skin exported."},
    {"Skin supprimé.", "Skin deleted."},
    {"Skin introuvable pour ce pseudo.", "No skin found for this username."},
    {"Sélectionne d'abord un skin.", "Select a skin first."},
    {"Récupération de ton skin officiel...", "Fetching your official skin..."},
    {"Aucun pseudo configuré (Compte - Microsoft).",
     "No username configured (Account - Microsoft)."},
    {"Erreur : copie impossible.", "Error: copy failed."},
    {"Erreur : telechargement impossible.", "Error: download failed."},
    {"Erreur : echec inattendu.", "Error: unexpected failure."},
    {"Pseudo invalide (3-16 caractères, lettres/chiffres/_).",
     "Invalid username (3-16 characters, letters/digits/_)."},
    {"1 skin(s) en ligne. Clique pour importer.",
     "1 online skin(s). Click to import."},
    {"Skin importé", "Skin imported"},
    {"Fichier non supporté", "Unsupported file"},
    {"Import non porté", "Import not ported"},
    {"Modpack CurseForge / Modrinth…", "CurseForge / Modrinth modpack…"},
    {"Lecture de l'archive…", "Reading the archive…"},
    {"Import annulé", "Import cancelled"},
    {"Import en cours", "Import running"},
    {"Modpack importé", "Modpack imported"},

    // ---------------- page Actualités ----------------
    {"Dernières actualités", "Latest news"},
    {"Historique des versions", "Version history"},
    {"Voir le site web", "Visit the website"},
    {"Aucune actualité pour l'instant.", "No news yet."},

    // ---------------- page Bedrock ----------------
    {"CHANGER DE MINECRAFT", "SWITCH MINECRAFT"},
    {"Minecraft Bedrock est installé sur ce PC",
     "Minecraft Bedrock is installed on this PC"},
    {"Minecraft Bedrock n'est pas installé", "Minecraft Bedrock is not installed"},
    {"Installe-le d'abord via le bouton Microsoft Store.",
     "Install it first via the Microsoft Store button."},
    {"Lancer Bedrock", "Launch Bedrock"},
    {"Microsoft Store", "Microsoft Store"},

    // ---------------- page Compte ----------------
    {"Mode d'authentification", "Authentication mode"},
    {"Compte Microsoft", "Microsoft account"},
    {"Mode Hors-ligne", "Offline mode"},
    {"Joueur Minecraft", "Minecraft player"},
    {"Choisir un pseudo", "Choose a username"},
    {"Pseudo...", "Username..."},
    {"Se connecter avec Microsoft", "Sign in with Microsoft"},
    {"Changer de compte", "Switch account"},
    {"Connexion officielle par code d'appareil : le launcher ouvre "
     "microsoft.com/link, tu entres le code affiché.",
     "Official device-code sign-in: the launcher opens microsoft.com/link, you "
     "enter the code shown."},
    {"Pseudo mis à jour", "Username updated"},
    {"Jeton supprimé de ce PC. Retour en mode hors ligne.",
     "Token removed from this PC. Back to offline mode."},

    // ---------------- modale d'auth Microsoft ----------------
    {"Connexion Microsoft", "Microsoft sign-in"},
    {"Ton navigateur s'est ouvert sur la page", "Your browser opened the"},
    {"de connexion Microsoft. Entre ce code :",
     "Microsoft sign-in page. Enter this code:"},
    {"Rouvrir la page", "Reopen the page"},
    {"Copier le code", "Copy the code"},
    {"Code copié", "Code copied"},
    {"Connexion en cours...", "Signing in..."},
    {"Cela peut prendre quelques secondes.", "This may take a few seconds."},
    {"Échec de la connexion Microsoft", "Microsoft sign-in failed"},
    {"Demande du code à Microsoft...", "Requesting the code from Microsoft..."},
    {"Connexion à Xbox Live...", "Connecting to Xbox Live..."},
    {"Autorisation XSTS...", "XSTS authorization..."},
    {"Obtention du jeton Minecraft...", "Getting the Minecraft token..."},
    {"Lecture du profil Minecraft...", "Reading the Minecraft profile..."},
    {"Entre ce code sur la page Microsoft qui vient de s'ouvrir.",
     "Enter this code on the Microsoft page that just opened."},
    {"Connecté à Microsoft", "Signed in to Microsoft"},
    {"Connexion Microsoft annulée.", "Microsoft sign-in cancelled."},

    // ---------------- page Paramètres ----------------
    {"Général", "General"},
    {"Apparence", "Appearance"},
    {"Intégrations", "Integrations"},
    {"Avancé", "Advanced"},
    {"Enregistrer les paramètres", "Save settings"},
    {"Couleurs par défaut", "Default colors"},
    {"Couleurs du launcher", "Launcher colors"},
    {"Fond", "Background"},
    {"Cartes / panneaux", "Cards / panels"},
    {"Accent (boutons)", "Accent (buttons)"},
    {"Retirer l'image", "Remove image"},
    {"Choisir une image...", "Choose an image..."},
    {"Aucune image de fond.", "No background image."},
    {"Image actuelle : ", "Current image: "},
    {"Vérifier maintenant", "Check now"},
    {"Vérification...", "Checking..."},
    {"Nom du joueur", "Player name"},
    {"Java (vide = recherche auto)", "Java (empty = auto-detect)"},
    {"Chemin de Java (vide = détection automatique)", "Java path (empty = auto-detect)"},
    {"Mémoire maximale allouée à Minecraft (Go)",
     "Maximum memory allocated to Minecraft (GB)"},
    {"Dossier des instances", "Instances folder"},
    {"Compter les FPS", "Show FPS counter"},
    {"Minimiser le launcher au lancement", "Minimize the launcher on launch"},
    {"Ouvrir le dossier de données", "Open the data folder"},
    {"Ouvrir launcher.log", "Open launcher.log"},
    {"Langue", "Language"},
    {"Français", "French"},
    {"English", "English"},
    {"ACTUALITÉS & LANGUE", "NEWS & LANGUAGE"},
    {"MISES À JOUR AUTOMATIQUES", "AUTOMATIC UPDATES"},
    {"CURSEFORGE", "CURSEFORGE"},
    {"TÉLÉMÉTRIE & LOGS DISTANTS", "TELEMETRY & REMOTE LOGS"},
    {"Maintenance", "Maintenance"},
    {"Diagnostic du système", "System diagnostics"},
    {"Libérer de l'espace (cache)", "Free up space (cache)"},
    {"Vérifier les mises à jour", "Check for updates"},
    {"Ouvrir la page de la version", "Open the release page"},
    {"URL des actualités (fichier JSON : title, date, tag, text)",
     "News URL (JSON file: title, date, tag, text)"},
    {"URL du flux de mises à jour (Velopack, optionnel)",
     "Update feed URL (Velopack, optional)"},
    {"Clé API CurseForge (console.curseforge.com - gratuite)",
     "CurseForge API key (console.curseforge.com - free)"},
    {"Version installée : ", "Installed version: "},
    {"Activer la Rich Presence Discord", "Enable Discord Rich Presence"},
    {"ID d'application Discord (discord.com/developers/applications)",
     "Discord Application ID (discord.com/developers/applications)"},
    {"Envoyer les rapports de crash et stats d'utilisation vers Discord",
     "Send crash reports and usage stats to Discord"},
    {"URL du webhook Discord", "Discord webhook URL"},
    {"Impossible de lire le journal.", "Cannot read the log."},
    {"Paramètres enregistrés.", "Settings saved."},
    {"Image de fond", "Background image"},
    {"Rich Presence Discord", "Discord Rich Presence"},
    {"format #rrggbb", "format #rrggbb"},

    // ---------------- messages / toasts ----------------
    {"Erreur", "Error"},
    {"Terminé", "Done"},
    {"Prêt", "Ready"},
};

using Map = std::unordered_map<std::string_view, const char*>;

const Map& dict() {
    static const Map m = [] {
        Map out;
        out.reserve(sizeof(kTable) / sizeof(kTable[0]) * 2);
        for (const auto& e : kTable) out.emplace(std::string_view(e.fr), e.en);
        return out;
    }();
    return m;
}

} // namespace

const char* current() {
    return DataStore::settings.language == "en" ? "en" : "fr";
}

bool is_en() { return DataStore::settings.language == "en"; }

void set_language(const char* code) {
    const std::string next = (code && std::strcmp(code, "en") == 0) ? "en" : "fr";
    if (DataStore::settings.language == next) return;
    DataStore::settings.language = next;
    DataStore::save();
}

const char* t(const char* fr, const char* en) {
    if (!is_en()) return fr;
    return en ? en : fr;
}

const char* t(const char* fr) {
    if (!fr || !is_en()) return fr;
    const auto& m = dict();
    const auto it = m.find(std::string_view(fr));
    return it == m.end() ? fr : it->second;
}

std::string t(const std::string& fr) {
    if (!is_en()) return fr;
    const auto& m = dict();
    const auto it = m.find(std::string_view(fr));
    return it == m.end() ? fr : std::string(it->second);
}

std::size_t dict_size() { return dict().size(); }

} // namespace tl::lang

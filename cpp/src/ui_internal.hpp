#pragma once

// Etat UI partage entre les fichiers de pages (ui_*.cpp) et ui.cpp.
// Header INTERNE : ne pas inclure depuis hors du module ui.

#include "ui.hpp"

#include "datastore.hpp"
#include "lang.hpp"
#include "apptasks.hpp"
#include "launch_flow.hpp"
#include "modcheck.hpp"

#include <imgui.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace tl::ui {

// ---------------------------------------------------------------------------
// Traduction (lang.hpp) — `tr` plutot que `t` : plusieurs pages ont deja des
// locales nommees `t`, qui masqueraient silencieusement la fonction.
// ---------------------------------------------------------------------------

inline const char* tr(const char* fr) { return lang::t(fr); }
inline const char* tr(const char* fr, const char* en) { return lang::t(fr, en); }
inline std::string tr(const std::string& fr) { return lang::t(fr); }

// ---------------------------------------------------------------------------
// Palette v5 (#0e0e13 / #15151b / #22222a / #f2f2f5 / #8a8a99 / #3b82f6)
// ---------------------------------------------------------------------------

// Teinte de survol d'une surface. Une couleur ecrite en dur ne peut convenir
// qu'a UN theme : « un peu plus clair » sur fond sombre devient « invisible »
// sur fond clair. On decale donc a partir de la luminance de la surface —
// on eclaircit ce qui est sombre, on assombrit ce qui est clair.
inline ImVec4 shade_by(const ImVec4& base, float amount) {
    // Luminance perceptuelle (Rec. 709) : le vert pese bien plus que le bleu.
    const float lum = 0.2126f * base.x + 0.7152f * base.y + 0.0722f * base.z;
    const float k = lum < 0.5f ? amount : -amount;
    auto c = [&](float v) {
        const float r = v + k;
        return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
    };
    return ImVec4(c(base.x), c(base.y), c(base.z), base.w);
}

inline ImVec4 hex(unsigned rgb, float a = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f,
                  (rgb & 0xFF) / 255.0f, a);
}

// Valeurs par defaut ; surchargeables depuis Parametres > Apparence
// (settings.bgColor / cardColor / accentColor, comme Theme.cs).
// Definies dans ui.cpp — `theme_reload()` les recalcule.
extern ImVec4 kBg;
extern ImVec4 kCard;
extern ImVec4 kBorder;
extern ImVec4 kText;
extern ImVec4 kDim;
extern ImVec4 kAccent;
extern ImVec4 kAccentHover;
extern ImVec4 kAccentActive;
extern ImVec4 kDanger;
extern ImVec4 kButton;
extern ImVec4 kButtonHover;
extern ImVec4 kButtonActive;

// Relit les 3 couleurs des reglages et recalcule les couleurs derivees.
// Sans surcharge, la palette v5 d'origine est conservee a l'identique.
void theme_reload();
void theme_apply_style(); // reporte la palette dans ImGuiStyle

// "#rrggbb" -> ImVec4 ; false si la chaine n'est pas une couleur.
bool parse_hex_color(const std::string& s, ImVec4& out);
std::string hex_of(const ImVec4& c);

// ---------------------------------------------------------------------------
// Etat partage (worker d'installation <-> boucle UI)
// ---------------------------------------------------------------------------

enum class Phase { Idle, Preparing, GameRunning, Error };

struct UiState {
    std::mutex m;
    // ecrits par le worker (sous m)
    std::string status = "Pret.";
    std::string stage;
    int done = 0, total = 0;
    bool resultReady = false;
    LaunchResult result;
    std::deque<std::string> logLines;

    // main thread uniquement
    Phase phase = Phase::Idle;
    std::string error;
    std::thread worker;
    GameProcess game{};
    bool gameActive = false;
    bool minimizedForGame = false;

    // atomiques
    std::atomic<bool> cancel{false};

    // instances (module 3bis)
    std::string selInstId;             // instance selectionnee (par Id)
    nlohmann::json autoInst = nullptr; // instance de test TL_AUTO_VERSION
    bool autoActive = false;
    bool ctxAuto = false; // TL_AUTO_CTX : ouvre le menu contextuel (test)
    int page = 0;    // 0 Accueil, 1 Instances, 2 Jouer, 3 Parametres
    // Onglet a ouvrir a la prochaine frame de la page Parametres (-1 =
    // laisser celui que l'utilisateur regardait). Consomme par
    // settings_page : les raccourcis de la page Aide arrivent ainsi
    // directement sur la bonne section.
    int settingsTab = -1;
    int sortIdx = 0; // 0 temps de jeu, 1 nom, 2 lancements, 3 recemment jouee
    bool smallGrid = true;
    char searchBuf[64] = "";
    // Tags : modale d'edition (saisie libre separee par des virgules) et
    // filtre actif de la liste ("" = pas de filtre).
    std::string tagsId;       // instance en cours d'edition
    char tagsBuf[256] = "";
    bool tagsRequest = false; // demande d'ouverture de la modale
    std::string tagFilter;
    // Actions du menu contextuel qui modifient UN lancement, consommees par
    // start_worker() puis remises a zero : elles ne doivent pas coller a
    // l'instance ni survivre au lancement suivant.
    bool launchVanillaOnce = false; // ignore le chargeur de mods
    bool launchRepairOnce = false;  // revalide tous les fichiers (SHA-1)
    // Instance pour laquelle l'utilisateur a choisi « Lancer quand même »
    // malgre un conflit de mods bloquant. Retenu jusqu'au prochain
    // demarrage du launcher : redemander a chaque lancement apres qu'il a
    // dit oui serait du harcelement, et il peut toujours relire le detail
    // dans l'onglet Mods.
    std::string compatSkipId;
    // Rapport depose par le worker quand il refuse de lancer. Lu et remis
    // a zero par poll_state, qui ouvre alors la modale : le worker ne
    // touche jamais a l'interface lui-meme.
    modcheck::Report compatGate;
    bool compatGateReady = false;

    // modales instance (1 creer, 2 editer, 3 supprimer)
    int modal = 0;
    bool modalRequest = false;
    std::string modalId;
    char instName[64] = "";
    char instDesc[256] = "";
    char instVer[64] = "latest";
    int instLoader = 0;
    std::map<std::string, std::pair<int, int>> counts; // id -> (mods, cartes)
    bool countsDirty = true;

    // stats de session (C# Play : Launches++ au depart, PlaySeconds/LastPlayed a la fin)
    std::string statInstId;
    bool statActive = false;
    std::chrono::steady_clock::time_point statStart{};

    // toast (fidele Notifier C# : 320x84, bordure accent, auto-close 4,5 s)
    std::string toastTitle, toastMsg;
    std::chrono::steady_clock::time_point toastUntil{};
};

extern UiState g;

extern const char* const kInstanceLoaders[];
extern ImFont* fBig;   // 24 px (titres, valeurs de stats)
extern ImFont* fSmall; // 12 px (metadonnees de cartes)
extern ImFont* fTiny;  // 10 px (ligne de compteurs, C# FontSize 10)

// ---------------------------------------------------------------------------
// Helpers communs (ui.cpp)
// ---------------------------------------------------------------------------

void push_log(const std::string& line);
void notify_toast(const std::string& title, const std::string& msg);
nlohmann::json& inst_array();
nlohmann::json* find_instance(const std::string& id);
bool inst_played(const nlohmann::json& e);
std::string inst_last_label(const nlohmann::json& e);
std::string format_playtime(long long s);
std::string format_date(const std::string& iso); // reglage DateFormat
nlohmann::json* selected_instance();
void refresh_counts(const std::string& id);
std::optional<std::string> pick_javaw();
void open_in_explorer(const std::filesystem::path& p);
std::string wstr_to_utf8(const wchar_t* w);
void notify_log(const std::string& msg);
std::optional<std::string> pick_zip_open();
std::optional<std::string> pick_model_file();
std::optional<std::string> pick_zip_save(const std::string& defaultName);
#ifdef _WIN32
std::optional<std::string> pick_folder(const wchar_t* title);
#else
std::optional<std::string> pick_folder(const char* title);
#endif
// Sélecteur générique (dialogs.cpp) : "Nom (*.a *.b)" + "*.a *.b".
// Utilisé par les pages ; les pickers historiques ci-dessus sont conservés.
std::optional<std::string> pick_file_open(const std::string& title,
                                          const std::string& filterName,
                                          const std::string& filterPat,
                                          const std::string& startFile = {});
std::vector<std::string> pick_files_open(const std::string& title,
                                         const std::string& filterName,
                                         const std::string& filterPat,
                                         const std::string& startDir = {});
std::optional<std::string> pick_file_save(const std::string& title,
                                          const std::string& filterName,
                                          const std::string& filterPat,
                                          const std::string& defaultName,
                                          const std::string& defExt = {});
void import_zip();
void import_folder();
void export_zip(const nlohmann::json& inst);
void duplicate_instance(const nlohmann::json& inst);
void start_worker();
void poll_state(SDL_Window* window);

// widgets
bool nav_button(const char* label, bool active);
bool accent_button(const char* label, const ImVec2& size);
bool danger_button(const char* label, const ImVec2& size);
bool icon_play_button(const char* id, float size);
void stat_card(const std::string& value, const char* label);
std::string trimmed(const char* s);

// ---------------------------------------------------------------------------
// Pages (un fichier ui_<page>.cpp chacune)
// ---------------------------------------------------------------------------

void home_page();        // ui_home.cpp
void instances_page();   // ui_instances.cpp
void open_edit_modal(const std::string& id);
void instance_modals();
void open_instance_detail(const std::string& id, int tab = 0);
void instance_detail_modal(); // ui_instancedetail.cpp
void play_page();        // ui_play.cpp
void settings_page();    // ui_settings.cpp
void settings_stop();    // ui_settings.cpp : joint le worker de maintenance
void explore_page();     // ui_explore.cpp (page 9)
void explorer_page();    // ui_explorer.cpp (page 10) : fichiers et mondes
void mapeditor_page();   // ui_mapeditor.cpp (page 11) : grille de chunks
void citygen_page();     // ui_citygen.cpp (page 12) : ville OpenStreetMap
void moddev_page();      // ui_moddev.cpp (page 13) : developpement de mods
void modelviewer_page(); // ui_modelviewer.cpp (page 14) : modeles 3D
void downloads_page();   // ui_downloads.cpp (page 15) : file de telechargements
void social_page();      // ui_social.cpp (page 16) : amis et messages
void help_page();        // ui_help.cpp (page 17) : aide et assistance
void help_stop();        // ui_help.cpp : joint le worker d'export

// Phase 5 — compatibilite des mods (ui_modcheck.cpp)
void modcheck_panel(const nlohmann::json& inst); // onglet Mods
void modcheck_invalidate();  // un mod a ete active, desactive ou supprime
void modcheck_gate_modal();  // barrage avant lancement (1x/frame)
void modcheck_open_gate(const std::string& instId,
                        const modcheck::Report& rep);
void modcheck_stop();        // joint le worker d'analyse
// Mises a jour des mods et leur changelog (ui_modupdate.cpp).
void modupdate_panel(const nlohmann::json& inst);
void modupdate_stop();

// Phase 8 (ui_plugins.cpp) : panneaux de la page Paramètres > Avancé.
void plugins_panel();
void localapi_panel();
void apikeys_panel();
void events_panel();
void jvmwarm_panel();
// Lance une recherche sur la page Exploration (ui_explore.cpp).
void explore_search(const std::string& term);

// Comparateur de modpacks (ui_compare.cpp) : deux instances cote a cote.
void compare_open(const std::string& leftInstanceId);
void compare_modal();  // 1x/frame
void compare_stop();   // joint le worker
// Phase 7 (ui_help.cpp), appeles depuis ui::init / ui::frame :
void whatsnew_init();    // decide si les notes de version sont a montrer
void autoupdate_init();  // verification periodique (UpdateCheckHours)
void whatsnew_modal();   // la modale, une fois par frame
void apply_startup_selection(); // reglage « Jeu au démarrage »
void search_frame();     // ui_search.cpp : palette de recherche (Ctrl+K)
void search_open();
bool onboarding_needed();  // ui_onboarding.cpp
void onboarding_frame();   // ui_onboarding.cpp : assistant de 1er lancement
void explore_stop();     // ui_explore.cpp : joint le worker

// Module 3ter — pages reseau
struct NewsEntry {
    std::string title, date, tag, text;
};
struct NewsState {
    std::mutex m;
    bool loading = false;
    bool loaded = false;
    std::vector<NewsEntry> news;     // flux distant (ou cache)
    std::vector<NewsEntry> changelog; // fichier local
    std::thread th;
    std::atomic<bool> cancel{false};
    int taskId = 0; // entree panneau AppTasks (0 = aucune)
};
extern NewsState newsState;

// Historique local (ui_news.cpp) : la page Aide montre les memes entrees
// dans « Quoi de neuf » que la page Actualités.
std::vector<NewsEntry> changelog_entries();

struct PingResult {
    bool ok = false;
    bool errored = false; // exception (host inconnu, troncature...)
    bool started = false; // deja envoye au worker (sinon "Ping en cours...")
    bool cancelled = false; // batch annule avant d'atteindre ce serveur
    int online = 0, max = 0;
    std::string version, motd;
};
// Ping SLP direct (ui_servers.cpp, expose pour les tests avec faux serveur).
PingResult query_slp(const std::string& address,
                     const std::atomic<bool>* cancel = nullptr);
// Depose un lot d'adresses a pinger (suivi panneau). Expose pour les tests.
void queue_pings(const std::vector<std::string>& addrs);
struct ServersState {
    std::mutex m;
    std::condition_variable cv;
    std::map<std::string, PingResult> pings; // adresse -> resultat
    std::vector<std::string> targets;        // file en attente de ping
    bool pinging = false;
    bool workPending = false;
    bool workerStarted = false;
    int refreshSeq = 0; // invalidation des resultats obsoletes
    std::thread th;
    std::atomic<bool> cancel{false};
    // Batch de ping suivi comme tache de fond (registre AppTasks) :
    // pingCancel est branche par apptasks_begin, les compteurs sont gardes
    // par m. Les trois suivants ne sont touches que sous verrou.
    std::atomic<bool> pingCancel{false};
    int pingTaskId = 0;
    int pingDone = 0, pingTotal = 0;
    int cityEditIdx = -1; // ville chargee dans le formulaire (Modifier)
};
extern ServersState serversState;

void news_page();      // ui_news.cpp
void account_page();   // ui_account.cpp
void bedrock_page();   // ui_bedrock.cpp
void servers_page();   // ui_servers.cpp
void servers_ptero_stop(); // ui_servers.cpp : joint le worker hébergé (shutdown)
void skins_page();     // ui_skins.cpp
void skins_stop();     // ui_skins.cpp : annule le worker + libere les textures
void skins_import_path(const char* path); // ui_skins.cpp : glisser-deposer

// Module 4d — import de modpacks (ui_packs.cpp)
void import_modpack_pick();                        // ouvre le selecteur de fichier
void import_modpack_start(const std::string& path); // glisser-deposer
void share_instance_start(const nlohmann::json& inst); // copie le pack
void import_shared_from_clipboard();                   // colle un pack
void packs_frame();                                // bandeau + resultat (1x/frame)
void packs_stop();                                 // joint le worker (shutdown)
bool packs_busy();

// Module 4 — auth Microsoft (ui_auth.cpp ; la modale est tl::auth::draw_login_modal)
void auth_sync();                    // applique une connexion reussie (pseudo, toast)
void open_url(const std::string& url); // ShellExecuteW sur une URL UTF-8

// Module 4 — registre de tâches de fond (ui_apptasks.cpp)
void apptasks_panel();   // panneau des tâches de fond
void apptasks_frame();   // panneau si >= 1 tâche (1x/frame)
const char* apptasks_state_label(tl::tasks::State s);

// Raccordement des pages aux workers (module « rebranchement AppTasks ») :
//   apptasks_begin(titre, statut, &annulationLocale) cree l'entree dans le
//     registre ET branche l'atomic que la page sonde deja dans son worker :
//     le bouton Annuler du panneau declenche aussi l'annulation locale
//     (relais fait chaque frame par apptasks_frame). nullptr = aucun branchem.
//   apptasks_end(id, message) clot l'entree : fail(message) si message non
//     vide, sinon finish(). Appele par le worker lui-meme (et non par la page)
//     pour que le panneau soit juste meme si l'utilisateur quitte la page.
// Les deux sont sans effet pour id <= 0. Les chaines sont deposees BRUTES
// (source) : le panneau traduit a l'affichage.
int apptasks_begin(const std::string& title, const std::string& status,
                   std::atomic<bool>* localCancel = nullptr);
void apptasks_end(int id, const std::string& message = "");

} // namespace tl::ui

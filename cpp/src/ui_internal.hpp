#pragma once

// Etat UI partage entre les fichiers de pages (ui_*.cpp) et ui.cpp.
// Header INTERNE : ne pas inclure depuis hors du module ui.

#include "ui.hpp"

#include "datastore.hpp"
#include "lang.hpp"
#include "launch_flow.hpp"

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
    int sortIdx = 0; // 0 temps de jeu, 1 nom, 2 lancements, 3 recemment jouee
    bool smallGrid = true;
    char searchBuf[64] = "";

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
nlohmann::json* selected_instance();
void refresh_counts(const std::string& id);
std::optional<std::string> pick_javaw();
void open_in_explorer(const std::filesystem::path& p);
std::string wstr_to_utf8(const wchar_t* w);
void notify_log(const std::string& msg);
std::optional<std::string> pick_zip_open();
std::optional<std::string> pick_zip_save(const std::string& defaultName);
std::optional<std::string> pick_folder(const wchar_t* title);
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
void play_page();        // ui_play.cpp
void settings_page();    // ui_settings.cpp
void settings_stop();    // ui_settings.cpp : joint le worker de maintenance

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
};
extern NewsState newsState;

struct PingResult {
    bool ok = false;
    bool errored = false; // exception (host inconnu, troncature...)
    bool started = false; // deja envoye au worker (sinon "Ping en cours...")
    int online = 0, max = 0;
    std::string version, motd;
};
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
    int cityEditIdx = -1; // ville chargee dans le formulaire (Modifier)
};
extern ServersState serversState;

void news_page();      // ui_news.cpp
void account_page();   // ui_account.cpp
void bedrock_page();   // ui_bedrock.cpp
void servers_page();   // ui_servers.cpp
void skins_page();     // ui_skins.cpp
void skins_stop();     // ui_skins.cpp : annule le worker + libere les textures
void skins_import_path(const char* path); // ui_skins.cpp : glisser-deposer

// Module 4d — import de modpacks (ui_packs.cpp)
void import_modpack_pick();                        // ouvre le selecteur de fichier
void import_modpack_start(const std::string& path); // glisser-deposer
void packs_frame();                                // bandeau + resultat (1x/frame)
void packs_stop();                                 // joint le worker (shutdown)
bool packs_busy();

// Module 4 — auth Microsoft (ui_auth.cpp ; la modale est tl::auth::draw_login_modal)
void auth_sync();                    // applique une connexion reussie (pseudo, toast)
void open_url(const std::string& url); // ShellExecuteW sur une URL UTF-8

} // namespace tl::ui

#include "ui_internal.hpp"

#include "apptasks.hpp" // tl::tasks::update/cancel (miroir lancement)
#include "backup.hpp"
#include "crash_analyzer.hpp"
#include "maintenance.hpp"
#include "admin.hpp"
#include "presence.hpp"
#include "shortcut.hpp"
#include "ms_auth.hpp"
#include "telemetry.hpp"
#include "util_str.hpp"
#include "util_zip.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifdef _WIN32
#include <Windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#else
// Etape 5 (Linux) : waitpid (poll jeu), le reste est parti dans dialogs.cpp.
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef _WIN32
#include <bcrypt.h> // (inutilisé aujourd'hui : new_guid vit dans datastore.cpp)
#endif

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

namespace tl::ui {

// ---------------------------------------------------------------------------
// Etat partage (definitions — declarations dans ui_internal.hpp)
// ---------------------------------------------------------------------------

UiState g;

// ---------------------------------------------------------------------------
// Palette (defauts v5) — surchargeable depuis Parametres > Apparence
// ---------------------------------------------------------------------------

ImVec4 kBg = hex(0x0e0e13);
ImVec4 kCard = hex(0x15151b);
ImVec4 kBorder = hex(0x22222a);
ImVec4 kText = hex(0xf2f2f5);
ImVec4 kDim = hex(0x8a8a99);
ImVec4 kAccent = hex(0x3b82f6);
ImVec4 kAccentHover = hex(0x5b9bf8);
ImVec4 kAccentActive = hex(0x2f6fd6);
ImVec4 kDanger = hex(0xef5b69);
ImVec4 kButton = hex(0x1e1e28);
ImVec4 kButtonHover = hex(0x262633);
ImVec4 kButtonActive = hex(0x1a122a);

namespace {

ImVec4 lighten(const ImVec4& c, float f) {
    return ImVec4(c.x + (1.0f - c.x) * f, c.y + (1.0f - c.y) * f,
                  c.z + (1.0f - c.z) * f, c.w);
}
ImVec4 darken(const ImVec4& c, float f) {
    return ImVec4(c.x * (1.0f - f), c.y * (1.0f - f), c.z * (1.0f - f), c.w);
}

// Anciennes valeurs du theme « Minecraft » a ignorer (Theme.LegacyColors C#).
bool legacy_color(const std::string& v) {
    static const char* const kLegacy[] = {"#141519", "#23262c", "#6fbf3f"};
    for (const char* l : kLegacy)
        if (strCaseCmp(v.c_str(), l) == 0) return true;
    return false;
}

} // namespace

bool parse_hex_color(const std::string& s, ImVec4& out) {
    std::string h = trimmed(s.c_str());
    if (!h.empty() && h[0] == '#') h.erase(0, 1);
    if (h.size() != 6) return false;
    for (char c : h)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    const unsigned v = std::strtoul(h.c_str(), nullptr, 16);
    out = hex(v);
    return true;
}

std::string hex_of(const ImVec4& c) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "#%02x%02x%02x",
                  static_cast<int>(c.x * 255.0f + 0.5f),
                  static_cast<int>(c.y * 255.0f + 0.5f),
                  static_cast<int>(c.z * 255.0f + 0.5f));
    return buf;
}

void theme_reload() {
    // 1. defauts v5 (aucune surcharge = rendu identique a avant)
    kBg = hex(0x0e0e13);
    kCard = hex(0x15151b);
    kBorder = hex(0x22222a);
    kAccent = hex(0x3b82f6);
    kAccentHover = hex(0x5b9bf8);
    kAccentActive = hex(0x2f6fd6);
    kButton = hex(0x1e1e28);
    kButtonHover = hex(0x262633);
    kButtonActive = hex(0x1a122a);

    const auto& s = DataStore::settings;
    auto custom = [](const std::string& v, ImVec4& dst) {
        return !v.empty() && !legacy_color(v) && parse_hex_color(v, dst);
    };

    // 2. surcharges : les couleurs derivees suivent la couleur choisie
    custom(s.bgColor, kBg);
    ImVec4 c;
    if (custom(s.cardColor, c)) {
        kCard = c;
        kBorder = lighten(c, 0.06f);
        kButton = lighten(c, 0.04f);
        kButtonHover = lighten(c, 0.09f);
        kButtonActive = darken(c, 0.20f);
    }
    if (custom(s.accentColor, c)) {
        kAccent = c;
        kAccentHover = lighten(c, 0.12f);
        kAccentActive = darken(c, 0.16f);
    }

    if (ImGui::GetCurrentContext()) theme_apply_style();
}

// Reporte la palette dans le style ImGui (appele par theme_reload()).
void theme_apply_style() {
    ImGuiStyle& style = ImGui::GetStyle();
    auto acc = [](float a) { return ImVec4(kAccent.x, kAccent.y, kAccent.z, a); };

    style.Colors[ImGuiCol_Text] = kText;
    style.Colors[ImGuiCol_TextDisabled] = kDim;
    style.Colors[ImGuiCol_WindowBg] = kBg;
    style.Colors[ImGuiCol_ChildBg] = kBg;
    style.Colors[ImGuiCol_PopupBg] = kCard;
    style.Colors[ImGuiCol_Border] = kBorder;
    style.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    style.Colors[ImGuiCol_FrameBg] = kCard;
    style.Colors[ImGuiCol_FrameBgHovered] = lighten(kCard, 0.03f);
    style.Colors[ImGuiCol_FrameBgActive] = kButtonActive;
    style.Colors[ImGuiCol_TitleBg] = kBg;
    style.Colors[ImGuiCol_TitleBgActive] = kBg;
    style.Colors[ImGuiCol_MenuBarBg] = kCard;
    style.Colors[ImGuiCol_ScrollbarBg] = kBg;
    style.Colors[ImGuiCol_ScrollbarGrab] = lighten(kCard, 0.09f);
    style.Colors[ImGuiCol_ScrollbarGrabHovered] = lighten(kCard, 0.13f);
    style.Colors[ImGuiCol_ScrollbarGrabActive] = acc(0.7f);
    style.Colors[ImGuiCol_CheckMark] = kAccent;
    style.Colors[ImGuiCol_SliderGrab] = kAccent;
    style.Colors[ImGuiCol_SliderGrabActive] = kAccentHover;
    style.Colors[ImGuiCol_Button] = kButton;
    style.Colors[ImGuiCol_ButtonHovered] = kButtonHover;
    style.Colors[ImGuiCol_ButtonActive] = kButtonActive;
    style.Colors[ImGuiCol_Header] = acc(0.45f);
    style.Colors[ImGuiCol_HeaderHovered] = acc(0.6f);
    style.Colors[ImGuiCol_HeaderActive] = kAccentActive;
    style.Colors[ImGuiCol_Separator] = kBorder;
    style.Colors[ImGuiCol_SeparatorHovered] = acc(0.6f);
    style.Colors[ImGuiCol_SeparatorActive] = kAccent;
    style.Colors[ImGuiCol_ResizeGrip] = kBorder;
    style.Colors[ImGuiCol_ResizeGripHovered] = acc(0.5f);
    style.Colors[ImGuiCol_ResizeGripActive] = kAccent;
    style.Colors[ImGuiCol_PlotLines] = kAccent;
    style.Colors[ImGuiCol_PlotHistogram] = kAccent;
    style.Colors[ImGuiCol_TableHeaderBg] = kCard;
    style.Colors[ImGuiCol_TableBorderStrong] = kBorder;
    style.Colors[ImGuiCol_TableBorderLight] = kBorder;
    style.Colors[ImGuiCol_NavHighlight] = kAccent;
    style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
}

const char* const kInstanceLoaders[] = {"Vanilla", "Forge", "Fabric", "NeoForge",
                                        "Quilt"};
ImFont* fBig = nullptr;  // 24 px (titres, valeurs de stats)
ImFont* fSmall = nullptr; // 12 px (metadonnees de cartes)
ImFont* fTiny = nullptr;  // 10 px (ligne de compteurs, C# FontSize 10)

NewsState newsState;
ServersState serversState;

void push_log(const std::string& line) {
    std::lock_guard<std::mutex> lk(g.m);
    g.logLines.push_back(line);
    while (g.logLines.size() > 400) g.logLines.pop_front();
}

// Fidele Notifier C# : toast bas-droite, auto-close 4,5 s
void notify_toast(const std::string& title, const std::string& msg) {
    g.toastTitle = title;
    g.toastMsg = msg;
    g.toastUntil = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(4500);
}

// ---------------------------------------------------------------------------
// Instances (module 3bis — fidele InstanceInfo C#)
// ---------------------------------------------------------------------------

nlohmann::json& inst_array() { return DataStore::settings.instances; }

nlohmann::json* find_instance(const std::string& id) {
    for (auto& e : inst_array())
        if (e.is_object() && e.value("Id", "") == id) return &e;
    return nullptr;
}

// new_guid() et make_instance() vivent desormais dans tl_core (datastore.cpp) :
// les importeurs de modpacks en ont besoin sans dependre de l'UI.

bool inst_played(const nlohmann::json& e) {
    const std::string d = e.value("LastPlayed", "");
    return d.size() >= 10 && d.substr(0, 10) != "0001-01-01";
}

std::string inst_last_label(const nlohmann::json& e) {
    if (!inst_played(e)) return tr("Jamais lancée", "Never launched");
    const std::string d = e.value("LastPlayed", "");
    // fr : jj/mm/aaaa ; en : mm/jj/aaaa
    const std::string dd = d.substr(8, 2), mm = d.substr(5, 2), yy = d.substr(0, 4);
    return lang::is_en() ? "Last session on " + mm + "/" + dd + "/" + yy
                         : "Dernière session le " + dd + "/" + mm + "/" + yy;
}

// C# HomePage.FormatPlayTime
std::string format_playtime(long long s) {
    if (s <= 0) return "0h";
    if (s < 3600) return std::to_string(s / 60) + "m";
    if (s >= 86400)
        return std::to_string(s / 86400) + tr("j ", "d ") +
               std::to_string((s % 86400) / 3600) + "h";
    return std::to_string(s / 3600) + "h";
}

// Instance lancee : auto (test) > selection > regle C# (plus recente jouee,
// sinon la premiere)
nlohmann::json* selected_instance() {
    if (g.autoActive) return &g.autoInst;
    if (!g.selInstId.empty())
        if (auto* e = find_instance(g.selInstId)) return e;
    auto& a = inst_array();
    if (!a.is_array() || a.empty()) return nullptr;
    const nlohmann::json* best = nullptr;
    for (const auto& e : a) {
        if (!e.is_object() || !inst_played(e)) continue;
        if (!best || e.value("LastPlayed", "") > best->value("LastPlayed", ""))
            best = &e;
    }
    if (!best)
        for (const auto& e : a)
            if (e.is_object()) {
                best = &e;
                break;
            }
    if (!best) return nullptr;
    g.selInstId = best->value("Id", "");
    return const_cast<nlohmann::json*>(best);
}

// C# CountFiles(mods, *.jar) + CountFolders(saves)
void refresh_counts(const std::string& id) {
    int mods = 0, maps = 0;
    std::error_code ec;
    const std::filesystem::path root = DataStore::instancesRoot() / id;
    const std::filesystem::path modsDir = root / "mods";
    if (std::filesystem::exists(modsDir, ec))
        for (const auto& f : std::filesystem::directory_iterator(modsDir, ec))
            if (f.is_regular_file() && f.path().extension() == ".jar") ++mods;
    const std::filesystem::path savesDir = root / "saves";
    if (std::filesystem::exists(savesDir, ec))
        for (const auto& f : std::filesystem::directory_iterator(savesDir, ec))
            if (f.is_directory()) ++maps;
    g.counts[id] = {mods, maps};
}

// ---------------------------------------------------------------------------
// Boites de dialogue (Win32 + xdg) : voir dialogs.cpp.
// ---------------------------------------------------------------------------

void notify_log(const std::string& msg) {
    push_log(msg);
    std::lock_guard<std::mutex> lk(g.m);
    g.status = msg;
}

void import_zip() {
    const auto path = pick_zip_open();
    if (!path) return;
    nlohmann::json inst = make_instance(
        std::filesystem::path(*path).stem().string(), "vanilla", "latest");
    const std::filesystem::path destDir =
        DataStore::instancesRoot() / inst.value("Id", "");
    std::error_code ec;
    std::filesystem::create_directories(destDir, ec);
    if (zip_extract_all(*path, destDir) < 0) {
        std::filesystem::remove_all(destDir, ec);
        notify_log(tr("Erreur d'import : archive illisible."));
        return;
    }
    inst_array().push_back(std::move(inst));
    DataStore::save();
    g.countsDirty = true;
    notify_log(tr("Import terminé : instance importée."));
}

void import_folder() {
#ifdef _WIN32
    const auto src = pick_folder(L"Importer un dossier comme instance");
#else
    const auto src = pick_folder("Importer un dossier comme instance");
#endif
    if (!src) return;
    nlohmann::json inst = make_instance(
        std::filesystem::path(*src).filename().string(), "vanilla", "latest");
    const std::filesystem::path destDir =
        DataStore::instancesRoot() / inst.value("Id", "");
    std::error_code ec;
    std::filesystem::copy(*src, destDir,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    if (ec) {
        notify_log(tr("Erreur d'import : ", "Import error: ") + ec.message());
        return;
    }
    inst_array().push_back(std::move(inst));
    DataStore::save();
    g.countsDirty = true;
    notify_log(tr("Import terminé : instance importée."));
}

void export_zip(const nlohmann::json& inst) {
    const std::string name = inst.value("Name", "instance");
    const auto out = pick_zip_save(name + ".zip");
    if (!out) return;
    const std::filesystem::path dir =
        DataStore::instancesRoot() / inst.value("Id", "");
    if (zip_create_from_dir(dir, *out)) {
        notify_log(tr("Export terminé : ", "Export complete: ") + std::filesystem::path(*out).filename().string());
    } else {
        notify_log(tr("Erreur d'export."));
    }
}

void duplicate_instance(const nlohmann::json& inst) {
    nlohmann::json clone = make_instance(inst.value("Name", "") + " (copie)",
                                         inst.value("Loader", "Vanilla"),
                                         inst.value("McVersion", "latest"));
    clone["Description"] = inst.value("Description", "");
    clone["MaxRamGb"] = inst.value("MaxRamGb", 0);
    clone["JvmArgs"] = inst.value("JvmArgs", "");
    const std::filesystem::path srcDir =
        DataStore::instancesRoot() / inst.value("Id", "");
    const std::filesystem::path destDir =
        DataStore::instancesRoot() / clone.value("Id", "");
    std::error_code ec;
    if (std::filesystem::is_directory(srcDir, ec))
        std::filesystem::copy(
            srcDir, destDir,
            std::filesystem::copy_options::recursive, ec);
    else
        std::filesystem::create_directories(destDir, ec);
    inst_array().push_back(std::move(clone));
    DataStore::save();
    g.countsDirty = true;
    notify_log("Dupliquée : « " + inst.value("Name", "") + " (copie) » créée.");
}

// ---------------------------------------------------------------------------
// Lancement
// ---------------------------------------------------------------------------

void start_worker() {
    if (g.phase == Phase::Preparing || g.phase == Phase::GameRunning) return;
    if (g.worker.joinable()) return; // jamais deux (result pas consomme)

    nlohmann::json* inst = selected_instance();
    if (!inst) {
        push_log(tr("Aucune instance - créez-en une depuis la page Instances."));
        std::lock_guard<std::mutex> lk(g.m);
        g.status = tr("Aucune instance - créez-en une depuis la page Instances.");
        return;
    }

    LaunchRequest req;
    req.version = inst->value("McVersion", "latest");
    req.loader = inst->value("Loader", "Vanilla");
    const std::string instId = inst->value("Id", "default");
    req.gameDir = (DataStore::instancesRoot() / instId).string();
    req.instanceId = instId;
    req.jvmArgs = inst->value("JvmArgs", "");
    req.ramGb = inst->value("MaxRamGb", 0);

    // C# Play() : Launches++ + Save avant le lancement.
    // Pas de stats pour l'instance de test (jamais ecrite dans le config).
    g.statActive = false;
    g.statInstId.clear();
    if (!g.autoActive) {
        (*inst)["Launches"] = inst->value("Launches", 0) + 1;
        DataStore::save();
        g.statInstId = instId;
        g.statActive = true;
        telemetry::report_launch(*inst); // silencieux sans webhook configure
    }
    // Rich Presence : « Joue à <instance> » + chrono de session.
    presence::set_game(*inst, req.joinServer);
    g.statStart = std::chrono::steady_clock::now();

    g.phase = Phase::Preparing;
    g.error.clear();
    g.cancel = false;
    {
        std::lock_guard<std::mutex> lk(g.m);
        g.status = "Préparation...";
        g.stage.clear();
        g.done = 0;
        g.total = 0;
        g.resultReady = false;
        g.result = LaunchResult{};
    }

    // Tache de fond (panneau + Annuler) : le relais panneau -> g.cancel est
    // fait chaque frame par apptasks_frame ; le worker cloture lui-meme.
    // tid est créé AVANT les lambdas ui qui le capturent.
    const std::string instName = inst->value("Name", "Minecraft");
    const int tid = apptasks_begin("Lancement de " + instName, "Préparation...",
                                   &g.cancel);

    LaunchUi ui;
    // Miroir vers le panneau AppTasks (progression 0..1, -1 si inconnue).
    ui.progress = [tid](const char* stage, int done, int total) {
        std::lock_guard<std::mutex> lk(g.m);
        if (stage) g.stage = stage;
        g.done = done;
        g.total = total;
        tl::tasks::update(tid, stage ? stage : "",
                          (done >= 0 && total > 0)
                              ? std::clamp(static_cast<double>(done) / total, 0.0, 1.0)
                              : -1.0);
    };
    ui.status = [tid](const char* s) {
        std::lock_guard<std::mutex> lk(g.m);
        if (s) g.status = s;
        if (s) tl::tasks::update(tid, s);
    };
    ui.log = [](const char* line) {
        if (line) push_log(line);
    };

    g.worker = std::thread([req, ui, tid] {
        LaunchResult res = launch_flow(req, ui, g.cancel);
        // Annulation (bouton Jouer ou panneau) : l'entree reste « Annulee ».
        if (res.cancelled || g.cancel.load()) (void)tl::tasks::cancel(tid);
        apptasks_end(tid, res.started ? std::string{} : res.error);
        std::lock_guard<std::mutex> lk(g.m);
        g.result = std::move(res);
        g.resultReady = true;
    });
}

// Consomme le resultat du worker / detecte la fin du jeu. Main thread.
namespace {
// Sonde non bloquante « jeu encore actif ? » : nullopt = oui (ou sonde
// impossible — sous Windows l'erreur gardait aussi le jeu « actif »).
std::optional<int> poll_game_exit(void* hProcess) {
    if (!hProcess) return std::nullopt;
#ifdef _WIN32
    DWORD code = 0;
    if (!GetExitCodeProcess(static_cast<HANDLE>(hProcess), &code)) return std::nullopt;
    if (code == STILL_ACTIVE) return std::nullopt;
    return static_cast<int>(code); // signe, comme C# ExitCode
#else
    int st = 0;
    const pid_t r = ::waitpid(static_cast<pid_t>(reinterpret_cast<intptr_t>(hProcess)),
                              &st, WNOHANG);
    if (r == 0) return std::nullopt; // actif
    if (r < 0) return -1;            // déjà moissonné : fini, code inconnu
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return -1; // signalé : fini, code inconnu
#endif
}
} // namespace

void poll_state(SDL_Window* window) {
    bool needJoin = false;
    bool startGame = false;
    LaunchResult result;
    {
        std::lock_guard<std::mutex> lk(g.m);
        if (g.phase == Phase::Preparing && g.resultReady) {
            result = std::move(g.result);
            g.resultReady = false;
            needJoin = true;
        }
    }
    if (needJoin) {
        if (g.worker.joinable()) g.worker.join();
        if (result.started) {
            g.game = result.proc;
            g.gameActive = true;
            g.phase = Phase::GameRunning;
            startGame = true;
            std::lock_guard<std::mutex> lk(g.m);
            g.status = tr("Minecraft en cours d'exécution...");
        } else if (result.cancelled) {
            g.statActive = false; // comme C# : PlaySeconds seulement si jeu demarre
            g.phase = Phase::Idle;
            std::lock_guard<std::mutex> lk(g.m);
            g.status = tr("Lancement annulé.");
        } else {
            g.statActive = false;
            g.phase = Phase::Error;
            g.error = result.error;
            std::lock_guard<std::mutex> lk(g.m);
            g.status = tr("Échec du lancement.");
        }
    }

    if (startGame && DataStore::settings.minimizeOnLaunch) {
        SDL_MinimizeWindow(window);
        g.minimizedForGame = true;
    }

    if (g.gameActive) {
        if (const auto code = poll_game_exit(g.game.hProcess)) {
            const int exitCode = *code;
            close_game(g.game);
            g.gameActive = false;
            g.phase = Phase::Idle;
            if (g.minimizedForGame) {
                SDL_RestoreWindow(window);
                g.minimizedForGame = false;
            }
            {
                std::lock_guard<std::mutex> lk(g.m);
                g.status = tr("Jeu fermé (code de sortie ", "Game closed (exit code ") + std::to_string(exitCode) + ").";
            }
            log_line("Jeu fermé (code de sortie " + std::to_string(exitCode) + ").");
            presence::set_launcher(); // retour a la presence « dans le launcher »

            // Instance concernee (statInstId reste valable jusqu'au bloc stats).
            const std::string instId =
                g.statActive ? g.statInstId
                             : (g.autoActive ? g.autoInst.value("Id", "")
                                             : g.selInstId);
            const std::filesystem::path gameDir =
                instId.empty() ? std::filesystem::path()
                               : DataStore::instancesRoot() / instId;

            if (exitCode != 0) {
                push_log("Le jeu s'est arrêté anormalement (code " +
                         std::to_string(exitCode) + "). Voir game-log.txt.");
                log_line("Le jeu s'est arrêté anormalement (code " +
                         std::to_string(exitCode) + ").");

                // Analyse de crash (C# : boite d'alerte ; ici un toast + journal)
                std::optional<std::string> advice;
                if (!gameDir.empty()) advice = crash::analyze_instance(gameDir);
                if (advice) {
                    std::string oneLine = *advice;
                    for (auto& ch : oneLine)
                        if (ch == '\n') ch = ' ';
                    log_line("Analyse de crash : " + oneLine);
                    push_log(oneLine);
                    notify_toast(tr("Minecraft s'est arrêté anormalement"), *advice);
                } else {
                    notify_toast(tr("Minecraft s'est arrêté"),
                                 tr("Code de sortie ", "Exit code ") +
                                     std::to_string(exitCode) +
                                     tr(". Voir le dossier de l'instance.",
                                        ". See the instance folder."));
                }

                // Telemetrie (non bloquante, silencieuse sans webhook configure)
                if (const auto* e = find_instance(instId)) {
                    const std::string tail =
                        gameDir.empty()
                            ? std::string()
                            : crash::tail_lines(gameDir / "game-log.txt", 30);
                    telemetry::report_crash(*e, exitCode, tail);
                }
            }

            // Sauvegarde automatique des mondes apres la partie (C#).
            if (!instId.empty()) {
                const std::string bk = backup::create(instId);
                if (!bk.empty()) log_line("Sauvegarde des mondes : " + bk);
            }
            // C# Play() : PlaySeconds += duree, LastPlayed = Now, Save
            if (g.statActive) {
                g.statActive = false;
                const long long secs =
                    std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::steady_clock::now() - g.statStart)
                        .count();
                if (auto* e = find_instance(g.statInstId)) {
                    (*e)["PlaySeconds"] = e->value("PlaySeconds", 0LL) + secs;
                    std::time_t t = std::time(nullptr);
                    std::tm tmv{};
#ifdef _WIN32
                    localtime_s(&tmv, &t);
#else
                    localtime_r(&t, &tmv);
#endif
                    char iso[32];
                    std::strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M:%S", &tmv);
                    (*e)["LastPlayed"] = iso;
                    const std::string nm = e->value("Name", "");
                    DataStore::save();
                    log_line("Jeu fermé. Temps de jeu ajouté à « " + nm + " ».");
                    push_log("Temps de jeu ajouté à « " + nm + " ».");
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------

bool nav_button(const char* label, bool active) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    }
    const bool clicked = ImGui::Button(label, ImVec2(-1.0f, 38.0f));
    if (active) ImGui::PopStyleColor(3);
    return clicked;
}

// ---------------------------------------------------------------------------
// Widgets de theme
// ---------------------------------------------------------------------------

bool accent_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

bool danger_button(const char* label, const ImVec2& size) {
    ImGui::PushStyleColor(ImGuiCol_Button, hex(0x7a2733));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hex(0x9c3340));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, hex(0x5e1e27));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    const bool r = ImGui::Button(label, size);
    ImGui::PopStyleColor(4);
    return r;
}

// Carre accent avec triangle de lecture (le glyphe "▶" C# manque au font)
bool icon_play_button(const char* id, float size) {
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    const bool clicked = ImGui::Button("##play", ImVec2(size, size));
    const ImVec2 p = ImGui::GetItemRectMin();
    const ImVec2 c(p.x + size * 0.44f, p.y + size * 0.5f);
    const float h = size * 0.5f;
    ImGui::GetWindowDrawList()->AddTriangleFilled(
        ImVec2(c.x - h * 0.30f, c.y - h * 0.42f),
        ImVec2(c.x - h * 0.30f, c.y + h * 0.42f),
        ImVec2(c.x + h * 0.44f, c.y),
        ImGui::ColorConvertFloat4ToU32(ImVec4(0.055f, 0.055f, 0.075f, 1)));
    ImGui::PopStyleColor(3);
    ImGui::PopID();
    return clicked;
}

void stat_card(const std::string& value, const char* label) {
    ImGui::BeginChild(label, ImVec2(170, 86), ImGuiChildFlags_Borders);
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(value.c_str());
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(label);
    ImGui::PopStyleColor();
    ImGui::EndChild();
}

std::string trimmed(const char* s) {
    std::string r = s;
    while (!r.empty() && (unsigned char)r.front() <= ' ') r.erase(r.begin());
    while (!r.empty() && (unsigned char)r.back() <= ' ') r.pop_back();
    return r;
}

// ---------------------------------------------------------------------------
// API publique
// ---------------------------------------------------------------------------

void init(SDL_Window*) {
    theme_reload(); // couleurs personnalisees avant le style ImGui
    telemetry::report_startup(); // C# ReportStartupAsync : 1 fois par session
    // Renouvellement silencieux de la session Microsoft (thread de fond) :
    // tant que l'utilisateur revient regulierement, il ne se reconnecte jamais.
    auth::startup_refresh();
    presence::set_launcher(); // Rich Presence Discord (sans effet si desactivee)
    admin::start();           // telemetrie d'administration (desactivee par defaut)
    // Raccourci bureau a la premiere ouverture (C# MainForm).
    ensure_desktop_shortcut();
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.SizePixels = 16.0f;
    io.Fonts->AddFontDefault(&cfg); // defaut
    ImFontConfig cfgBig;
    cfgBig.SizePixels = 24.0f;
    fBig = io.Fonts->AddFontDefault(&cfgBig); // titres / stats
    ImFontConfig cfgSmall;
    cfgSmall.SizePixels = 12.0f;
    fSmall = io.Fonts->AddFontDefault(&cfgSmall); // meta cartes
    ImFontConfig cfgTiny;
    cfgTiny.SizePixels = 10.0f;
    fTiny = io.Fonts->AddFontDefault(&cfgTiny); // compteurs cartes

    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 6.0f;
    style.GrabRounding = 6.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = ImVec2(14, 14);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 8);
    style.ScrollbarRounding = 6.0f;

    theme_apply_style();
}

void frame(SDL_Window* window) {
    poll_state(window);

    // Hooks d'automatisation de test : TL_AUTO_VERSION / TL_AUTO_PLAY / TL_AUTO_PAGE
    static bool autoInit = false;
    static bool autoPlayPending = false;
    if (!autoInit) {
        autoInit = true;
        // Nettoyage : dossier jetable d'un test precedent (si le jeu de test
        // a survecu a la fermeture du launcher, il n'a pas pu etre supprime).
        {
            std::error_code ec;
            std::filesystem::remove_all(DataStore::instancesRoot() / "tl_auto",
                                        ec);
        }
        if (const char* v = std::getenv("TL_AUTO_VERSION")) {
            // Instance de test : jamais ecrite dans le config utilisateur.
            g.autoInst = make_instance(std::string("Test ") + v, "Vanilla", v);
            g.autoInst["Id"] = "tl_auto";
            g.autoActive = true;
            push_log(std::string("[test] version forcee : ") + v);
        }
        autoPlayPending = std::getenv("TL_AUTO_PLAY") != nullptr;
        if (const char* p = std::getenv("TL_AUTO_PAGE")) g.page = std::atoi(p);
        if (std::getenv("TL_AUTO_CTX")) g.ctxAuto = true;
        // Auth Microsoft : ouvre la modale de connexion sans clic (test).
        if (std::getenv("TL_AUTO_LOGIN")) auth::login_start(/*force=*/true);
        if (const char* m = std::getenv("TL_AUTO_MODAL")) {
            g.modal = std::atoi(m);
            if (g.modal >= 1) {
                auto& arr = inst_array();
                for (auto& e : arr)
                    if (e.is_object()) {
                        g.modalId = e.value("Id", "");
                        if (g.modal == 2) {
                            std::snprintf(g.instName, sizeof(g.instName), "%s",
                                          e.value("Name", "").c_str());
                            std::snprintf(g.instVer, sizeof(g.instVer), "%s",
                                          e.value("McVersion", "latest").c_str());
                        }
                        break;
                    }
                g.modalRequest = true;
            }
        }
    }
    if (autoPlayPending && g.phase == Phase::Idle) {
        autoPlayPending = false;
        push_log("[test] lancement automatique");
        start_worker();
    }

    // Entree sur la page Instances -> rafraichir les compteurs mods/cartes
    static int prevPage = -1;
    if (prevPage != 1 && g.page == 1) g.countsDirty = true;
    prevPage = g.page;

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    ImGui::Begin("##root", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    // ---- Sidebar ----
    ImGui::BeginChild("##side", ImVec2(170, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("TEAM");
    ImGui::TextUnformatted("LAUNCHER");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("v%s", TL_VERSION_STRING);
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Spacing();

    if (nav_button(tr("Accueil"), g.page == 0)) g.page = 0;
    if (nav_button(tr("Instances"), g.page == 1)) g.page = 1;
    // Exploration porte l'index 9 : les index 0-8 etaient deja documentes
    // (TL_AUTO_PAGE) et sont laisses stables. L'ordre visuel est independant.
    if (nav_button(tr("Exploration"), g.page == 9)) g.page = 9;
    if (nav_button(tr("Explorateur"), g.page == 10)) g.page = 10;
    if (nav_button(tr("Édition de carte"), g.page == 11)) g.page = 11;
    if (nav_button(tr("Ville OSM"), g.page == 12)) g.page = 12;
    if (nav_button(tr("Mods (dev)"), g.page == 13)) g.page = 13;
    if (nav_button(tr("Modèles 3D"), g.page == 14)) g.page = 14;
    if (nav_button(tr("Jouer"), g.page == 2)) g.page = 2;
    if (nav_button(tr("Serveurs"), g.page == 3)) g.page = 3;
    if (nav_button(tr("Skins"), g.page == 4)) g.page = 4;
    if (nav_button(tr("Actualités"), g.page == 5)) g.page = 5;
    if (nav_button(tr("Bedrock"), g.page == 6)) g.page = 6;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    if (nav_button(tr("Compte"), g.page == 7)) g.page = 7;
    if (nav_button(tr("Paramètres"), g.page == 8)) g.page = 8;

    ImGui::EndChild();
    ImGui::SameLine();

    // ---- Page ----
    ImGui::BeginChild("##page", ImVec2(0, 0));
    switch (g.page) {
    case 0: home_page(); break;
    case 1: instances_page(); break;
    case 2: play_page(); break;
    case 3: servers_page(); break;
    case 4: skins_page(); break;
    case 5: news_page(); break;
    case 6: bedrock_page(); break;
    case 7: account_page(); break;
    case 9: explore_page(); break;
    case 10: explorer_page(); break;
    case 11: mapeditor_page(); break;
    case 12: citygen_page(); break;
    case 13: moddev_page(); break;
    case 14: modelviewer_page(); break;
    default: settings_page(); break;
    }
    ImGui::EndChild();

    // ---- Modales instance (contexte racine) ----
    instance_modals();
    instance_detail_modal();

    // ---- Tâches de fond (panneau si >= 1 tâche) ----
    apptasks_frame();

    ImGui::End();

    // ---- Assistant de premier lancement (avant tout le reste) ----
    onboarding_frame();

    // ---- Auth Microsoft : applique le succes puis dessine la modale ----
    auth_sync();
    auth::draw_login_modal();

    // ---- FPS (optionnel) ----
    if (DataStore::settings.fpsCounterEnabled) {
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        char fps[32];
        std::snprintf(fps, sizeof(fps), "%.0f FPS", ImGui::GetIO().Framerate);
        dl->AddText(ImVec2(ImGui::GetIO().DisplaySize.x - 70, 8),
                    ImGui::ColorConvertFloat4ToU32(kDim), fps);
    }

    // ---- Toast (Notifier C# : 320x84 bas-droite, bordure accent 2px) ----
    if (std::chrono::steady_clock::now() < g.toastUntil &&
        !g.toastMsg.empty()) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        const ImVec2 t0(ds.x - 320.0f - 16.0f, ds.y - 84.0f - 16.0f);
        const ImVec2 t1(ds.x - 16.0f, ds.y - 16.0f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(t0, t1, ImGui::ColorConvertFloat4ToU32(kCard), 8.0f);
        dl->AddRect(t0, t1, ImGui::ColorConvertFloat4ToU32(kAccent), 8.0f, 0,
                    2.0f);
        dl->AddText(ImVec2(t0.x + 12.0f, t0.y + 10.0f),
                    ImGui::ColorConvertFloat4ToU32(kText),
                    g.toastTitle.c_str());
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(t0.x + 12.0f, t0.y + 34.0f),
                    ImGui::ColorConvertFloat4ToU32(kDim),
                    g.toastMsg.c_str(), nullptr, t1.x - t0.x - 24.0f);
    }
}

void shutdown() {
    const bool dbg = std::getenv("TL_DEBUG_SHUTDOWN") != nullptr;
    if (dbg) std::fprintf(stderr, "SH: ui.cancel\n");
    g.cancel = true;
    if (dbg) std::fprintf(stderr, "SH: ui.join worker (joinable=%d)\n",
                          g.worker.joinable() ? 1 : 0);
    if (g.worker.joinable()) g.worker.join();
    if (dbg) std::fprintf(stderr, "SH: ui.join done\n");
    if (g.gameActive) {
        if (dbg) std::fprintf(stderr, "SH: ui.close_game\n");
        close_game(g.game);
        g.gameActive = false;
        if (dbg) std::fprintf(stderr, "SH: ui.close_game done\n");
    }
    // Instance de test : dossier jetable, jamais conserve
    if (g.autoActive) {
        std::error_code ec;
        std::filesystem::remove_all(DataStore::instancesRoot() / "tl_auto", ec);
    }
    // Pages reseau : annuler + joindre les workers (sinon std::terminate au dtor)
    newsState.cancel = true;
    if (newsState.th.joinable()) newsState.th.join();
    serversState.cancel = true;
    serversState.cv.notify_all(); // reveille le worker en attente de ping
    if (serversState.th.joinable()) serversState.th.join();
    // Serveurs hébergés (Pterodactyl) : joint le worker dédié.
    if (dbg) std::fprintf(stderr, "SH: servers_ptero_stop\n");
    servers_ptero_stop();
    if (dbg) std::fprintf(stderr, "SH: servers_ptero_stop done\n");
    // Page Skins : annuler + joindre le worker, liberer les textures GL
    // (contexte GL encore courant : ImGui/SDL sont detruits apres ici).
    if (dbg) std::fprintf(stderr, "SH: skins_stop\n");
    skins_stop();
    if (dbg) std::fprintf(stderr, "SH: skins_stop done\n");
    // Auth Microsoft : annule l'attente du device code et joint le thread.
    if (dbg) std::fprintf(stderr, "SH: auth_stop\n");
    auth::stop();
    if (dbg) std::fprintf(stderr, "SH: auth_stop done\n");
    // Rich Presence : efface la presence et joint le thread.
    if (dbg) std::fprintf(stderr, "SH: presence_stop\n");
    presence::shutdown();
    if (dbg) std::fprintf(stderr, "SH: presence_stop done\n");
    // Page Exploration : annuler la recherche/installation et joindre.
    if (dbg) std::fprintf(stderr, "SH: explore_stop\n");
    explore_stop();
    if (dbg) std::fprintf(stderr, "SH: explore_stop done\n");
    // Import de modpack : annuler et joindre le worker.
    if (dbg) std::fprintf(stderr, "SH: packs_stop\n");
    packs_stop();
    if (dbg) std::fprintf(stderr, "SH: packs_stop done\n");
    // Maintenance (diagnostic / mise a jour) : joindre avant de sortir.
    if (dbg) std::fprintf(stderr, "SH: settings_stop\n");
    settings_stop();
    if (dbg) std::fprintf(stderr, "SH: settings_stop done\n");
    // Telemetrie d'administration : arrete le battement.
    if (dbg) std::fprintf(stderr, "SH: admin_stop\n");
    admin::stop();
    if (dbg) std::fprintf(stderr, "SH: admin_stop done\n");
    // Telemetrie : vide la file d'envoi et joint le worker.
    if (dbg) std::fprintf(stderr, "SH: telemetry_stop\n");
    telemetry::stop();
    if (dbg) std::fprintf(stderr, "SH: telemetry_stop done\n");
}

// Glisser-deposer (SDL_DROPFILE) : skins pris en charge, modpacks signales.
void on_drop_file(const char* path) { skins_import_path(path); }

} // namespace tl::ui

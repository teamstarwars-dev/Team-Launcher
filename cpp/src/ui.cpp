#include "ui_internal.hpp"

#include "presets.hpp"
#include "social.hpp"
#include "apievents.hpp"
#include "apikeys.hpp"
#include "jvmwarm.hpp"
#include "localapi.hpp"
#include "modcheck.hpp"
#include "startup.hpp"
#include "support.hpp"

#include "fonts.hpp"
#include "downloads.hpp"
#include "netcache.hpp"
#include "icons.hpp"

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
    const auto& s = DataStore::settings;

    // 1. palette de base selon la variante choisie. « classic » reprend
    //    exactement les valeurs d'avant : aucune surcharge ne change le
    //    rendu historique.
    if (s.theme == "light") {
        // Clair : le texte devient sombre. kText et kDim etaient jusqu'ici
        // figes en clair-sur-sombre, ce qui rendait tout theme clair
        // illisible — ils font maintenant partie de la palette.
        kBg = hex(0xf4f4f7);
        kCard = hex(0xffffff);
        kBorder = hex(0xd8d8e0);
        kText = hex(0x1b1b22);
        kDim = hex(0x6b6b78);
        kAccent = hex(0x2563eb);
        kAccentHover = hex(0x3b82f6);
        kAccentActive = hex(0x1d4ed8);
        kDanger = hex(0xd23b49);
        kButton = hex(0xe9e9f0);
        kButtonHover = hex(0xdedee8);
        kButtonActive = hex(0xcdcdda);
    } else {
        kBg = hex(0x0e0e13);
        kCard = hex(0x15151b);
        kBorder = hex(0x22222a);
        kText = hex(0xf2f2f5);
        kDim = hex(0x8a8a99);
        kAccent = hex(0x3b82f6);
        kAccentHover = hex(0x5b9bf8);
        kAccentActive = hex(0x2f6fd6);
        kDanger = hex(0xef5b69);
        kButton = hex(0x1e1e28);
        kButtonHover = hex(0x262633);
        kButtonActive = hex(0x1a122a);
    }

    // 2. mode daltonisme : bleu et rouge sont la paire la plus courante de
    //    l'interface (accent / danger), et c'est justement celle que les
    //    deuteranopies et protanopies confondent le moins mal — mais le
    //    rouge « framboise » par defaut vire au brun. On passe a la palette
    //    Okabe-Ito, concue pour rester distinguable dans les trois formes
    //    de daltonisme : bleu ciel pour l'accent, vermillon pour le danger.
    //    Applique AVANT les couleurs personnalisees : un choix explicite de
    //    l'utilisateur reste prioritaire.
    if (s.colorblind) {
        kAccent = hex(0x0072B2);      // bleu Okabe-Ito
        kAccentHover = hex(0x2A8FCC);
        kAccentActive = hex(0x005A8D);
        kDanger = hex(0xD55E00);      // vermillon Okabe-Ito
    }

    auto custom = [](const std::string& v, ImVec4& dst) {
        return !v.empty() && !legacy_color(v) && parse_hex_color(v, dst);
    };

    // 3. surcharges de l utilisateur : les couleurs derivees suivent
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

// Date ISO (« aaaa-mm-jj... ») -> format choisi dans les reglages. Le
// format suivait la langue ; il en est maintenant independant, parce que
// l'un ne determine pas l'autre : on peut lire le launcher en francais et
// vouloir des dates ISO.
std::string format_date(const std::string& iso) {
    if (iso.size() < 10) return iso;
    const std::string yy = iso.substr(0, 4), mm = iso.substr(5, 2),
                      dd = iso.substr(8, 2);
    const std::string& f = DataStore::settings.dateFormat;
    if (f == "yyyy-MM-dd") return yy + "-" + mm + "-" + dd;
    if (f == "MM/dd/yyyy") return mm + "/" + dd + "/" + yy;
    return dd + "/" + mm + "/" + yy; // defaut : dd/MM/yyyy
}

std::string inst_last_label(const nlohmann::json& e) {
    if (!inst_played(e)) return tr("Jamais lancée", "Never launched");
    const std::string d = format_date(e.value("LastPlayed", ""));
    return lang::is_en() ? "Last session on " + d
                         : "Dernière session le " + d;
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
    // « Lancer en vanilla » : on force le chargeur a Vanilla sans toucher au
    // moindre fichier. Sans chargeur, Minecraft ignore purement et
    // simplement le dossier mods/ — inutile de le renommer, donc aucun
    // risque pour les donnees de l'utilisateur.
    req.loader = g.launchVanillaOnce ? std::string("Vanilla")
                                     : inst->value("Loader", "Vanilla");
    req.forceVerify = g.launchRepairOnce;

    // Profil de lancement actif : il surcharge la memoire et les arguments
    // JVM de l'instance. Les mods, eux, sont appliques au moment ou l'on
    // choisit le profil et non ici : renommer des fichiers juste avant de
    // lancer laisserait l'instance dans un etat imprevisible si le
    // lancement echouait entre-temps.
    if (const std::string pn = presets::active(*inst); !pn.empty()) {
        for (const auto& p : presets::load(*inst))
            if (p.name == pn) {
                if (p.ramGb > 0) req.ramGb = p.ramGb;
                if (!p.jvmArgs.empty()) req.jvmArgs = p.jvmArgs;
                push_log(tr("Profil : ", "Profile: ") + pn);
                break;
            }
    }
    // Consommes ici : ce sont des actions ponctuelles, elles ne doivent pas
    // s'appliquer au lancement suivant.
    g.launchVanillaOnce = false;
    g.launchRepairOnce = false;
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
        // Derniere instance lancee : c'est elle que « Jeu au démarrage :
        // le dernier joué » resélectionnera a la prochaine ouverture.
        DataStore::settings.lastGameId = instId;
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

    // Barrage de compatibilite (phase 5). Il tourne DANS le worker et non
    // ici : ouvrir une archive par mod prend le temps qu'il faut, et le
    // faire sur le fil de l'interface figerait la fenetre juste au moment
    // ou l'on vient de cliquer sur Jouer.
    //
    // « latest » n'est pas une version de Minecraft : la comparer aux
    // contraintes des mods les declarerait tous incompatibles. On passe
    // alors une chaine vide, que modcheck comprend comme « ne verifie pas
    // la version », et les autres controles (chargeur, doublons,
    // dependances) restent actifs.
    const std::string mcForCheck =
        (req.version == "latest" || req.version == "release" ||
         req.version == "snapshot")
            ? std::string{}
            : req.version;
    // Lancement en vanilla force : le dossier mods/ ne sera pas lu, il n'y
    // a donc rien a verifier.
    const bool checkMods = !g.autoActive && req.loader != "Vanilla" &&
                           g.compatSkipId != instId;
    const std::string checkLoader = req.loader;
    const std::string checkId = instId;

    g.worker = std::thread([req, ui, tid, checkMods, checkLoader, mcForCheck,
                            checkId] {
        if (checkMods) {
            auto rep = modcheck::scan(
                std::filesystem::path(req.gameDir) / "mods", checkLoader,
                mcForCheck);
            if (rep.errors > 0) {
                apptasks_end(tid, "Conflit de mods détecté.");
                LaunchResult stop;
                stop.started = false;
                stop.error = "Conflit de mods : " +
                             std::to_string(rep.errors) +
                             " problème(s) bloquant(s).";
                std::lock_guard<std::mutex> lk(g.m);
                g.compatGate = std::move(rep);
                g.compatGateReady = true;
                g.result = std::move(stop);
                g.resultReady = true;
                return;
            }
        }
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
        // Refus pour conflit de mods : on ouvre le detail plutot que de
        // laisser « Échec du lancement » sans explication.
        bool gate = false;
        modcheck::Report rep;
        {
            std::lock_guard<std::mutex> lk(g.m);
            gate = g.compatGateReady;
            if (gate) {
                rep = std::move(g.compatGate);
                g.compatGate = modcheck::Report{};
                g.compatGateReady = false;
            }
        }
        if (gate) {
            const nlohmann::json* inst = selected_instance();
            modcheck_open_gate(inst ? inst->value("Id", "") : std::string{},
                               rep);
        }
    }

    // --- Événement : la partie démarre -------------------------------
    // Émis ici et non dans start_worker : à ce point seulement le
    // processus du jeu tourne réellement. L'annoncer plus tôt mentirait
    // sur un lancement qui peut encore échouer à l'installation.
    if (startGame) {
        nlohmann::json ev;
        if (const auto* e = selected_instance())
            ev["instance"] = {{"id", e->value("Id", "")},
                              {"name", e->value("Name", "")},
                              {"loader", e->value("Loader", "")},
                              {"mcVersion", e->value("McVersion", "")}};
        events::emit(events::kGameStart, ev);
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

                // Analyse de crash (C# : boite d'alerte ; ici un toast + journal).
                // Le rapport structuré permet de servir deux publics d'un
                // coup : la bulle reçoit la phrase qui tient en 84 pixels,
                // le journal reçoit tout — mod en cause, fichier d'origine
                // et la ligne de pile qui l'accuse, pour qu'on puisse
                // vérifier plutôt que croire.
                const crash::Report rep =
                    gameDir.empty() ? crash::Report{}
                                    : crash::analyze_instance_report(gameDir);
                if (rep.found) {
                    std::string oneLine = rep.summary;
                    for (auto& ch : oneLine)
                        if (ch == '\n') ch = ' ';
                    log_line("Analyse de crash : " + oneLine);
                    push_log(rep.summary);
                    std::string toast = rep.title;
                    if (!rep.suspects.empty()) {
                        const auto& s = rep.suspects[0];
                        toast += "\n";
                        toast += s.source.empty() ? s.modId
                                                  : s.modId + " — " + s.source;
                    } else if (!rep.action.empty()) {
                        toast += "\n" + rep.action;
                    }
                    notify_toast(tr("Minecraft s'est arrêté anormalement"), toast);
                } else {
                    notify_toast(tr("Minecraft s'est arrêté"),
                                 tr("Code de sortie ", "Exit code ") +
                                     std::to_string(exitCode) +
                                     tr(". Voir le dossier de l'instance.",
                                        ". See the instance folder."));
                }

                // --- Événement sortant : le crash DÉJÀ ANALYSÉ -------------
                // C'est tout l'intérêt de la chose : le destinataire n'a
                // pas à savoir lire un rapport de crash, il reçoit le
                // verdict. Émis même sans diagnostic — un crash que nous
                // n'avons pas su reconnaître est précisément celui qu'on
                // veut voir passer. Sans abonné, emit() ne fait rien, pas
                // même créer un fil.
                {
                    nlohmann::json ev{{"exitCode", exitCode}};
                    if (const auto* e = find_instance(instId))
                        ev["instance"] = {
                            {"id", instId},
                            {"name", e->value("Name", "")},
                            {"loader", e->value("Loader", "")},
                            {"mcVersion", e->value("McVersion", "")}};
                    nlohmann::json diag{{"found", rep.found},
                                        {"cause", crash::cause_key(rep.cause)},
                                        {"title", rep.title},
                                        {"action", rep.action},
                                        {"summary", rep.summary}};
                    diag["suspects"] = nlohmann::json::array();
                    for (const auto& s : rep.suspects)
                        diag["suspects"].push_back({{"modId", s.modId},
                                                    {"version", s.version},
                                                    {"source", s.source},
                                                    {"score", s.score}});
                    ev["diagnosis"] = std::move(diag);
                    events::emit(events::kCrash, ev);
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

            // --- Événement : fin normale de partie ------------------
            // Le cas anormal est déjà parti plus haut, avec son
            // diagnostic : émettre les deux ferait deux messages pour un
            // seul plantage.
            if (exitCode == 0) {
                nlohmann::json ev{{"exitCode", exitCode}};
                if (const auto* e = find_instance(instId))
                    ev["instance"] = {{"id", instId},
                                      {"name", e->value("Name", "")},
                                      {"loader", e->value("Loader", "")},
                                      {"mcVersion", e->value("McVersion", "")}};
                if (g.statActive)
                    ev["playSeconds"] =
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::steady_clock::now() - g.statStart)
                            .count();
                events::emit(events::kGameStop, ev);
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

// Sortie demandee par l'interface (page Aide) : la boucle principale la
// lit apres la frame. Un booleen suffit — il n'est touche que par le fil
// de l'interface.
namespace {
bool s_quitRequested = false;
}

void request_quit() { s_quitRequested = true; }
bool quit_requested() { return s_quitRequested; }

bool handle_close_request(SDL_Window* window) {
    // Seul le reglage decide. Une preparation en cours n'est pas une
    // raison de refuser : shutdown() annule et joint proprement le worker,
    // c'est deja le comportement eprouve.
    if (DataStore::settings.closeBehavior != "minimize") return true;
    if (window) SDL_MinimizeWindow(window);
    // Dit une fois par session pourquoi la fenetre n'a pas disparu : sans
    // icone de zone de notification, une fenetre qui « refuse » de se
    // fermer passe autrement pour une panne.
    static bool told = false;
    if (!told) {
        told = true;
        notify_toast(tr("Launcher réduit", "Launcher minimised"),
                     tr("Pour quitter : page Aide, ou Paramètres > Général "
                        "> Démarrage et fermeture.",
                        "To quit: Help page, or Settings > General > "
                        "Startup and closing."));
    }
    return false;
}

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
    // Polices : police du systeme plutot que le ProggyClean embarque dans
    // ImGui — plus lisible aux grands corps, et surtout elle possede les
    // glyphes de ponctuation (« ... », tirets) qui s'affichaient en « ? ».
    fonts::build(DataStore::settings.uiFont, DataStore::settings.fontScale);

    // Phase 7 : notes de version (si la version a change depuis la derniere
    // execution) et instance preselectionnee au demarrage.
    whatsnew_init();
    apply_startup_selection();
    autoupdate_init();

    // API HTTP locale : seulement si un port a été posé. Un échec (port
    // déjà pris) n'empêche pas le launcher de démarrer — la page
    // Paramètres affichera la raison.
    // Abonnements aux événements : les hôtes doivent être joignables dès
    // le premier crash, sans attendre qu'on rouvre les réglages.
    events::start();

    if (DataStore::settings.localApiPort > 0 &&
        !DataStore::settings.localApiToken.empty()) {
        std::string err;
        if (!localapi::start(DataStore::settings.localApiPort,
                             DataStore::settings.localApiToken, &err))
            push_log("API locale : " + err);
    }

    // Le reglage « telechargements simultanes » existait dans la
    // configuration mais n etait lu nulle part : la file l applique.
    downloads::set_limit(DataStore::settings.maxDownloads);
    // File d'attente reprise apres une fermeture ou un plantage : un
    // modpack interrompu au 80e mod ne doit pas etre a refaire a la main.
    // Apres set_limit, sinon les elements repris partiraient tous a la
    // fois avec la limite par defaut.
    downloads::load_state();

    // Minuteur de sauvegarde automatique. Il lit le reglage a chaque
    // reveil, donc le demarrer meme quand l intervalle est nul : changer
    // le reglage prend effet sans redemarrer le launcher.
    backup::auto_start();

    // Liaison Discord pour le social (amis, messages). Sans effet si le
    // SDK n est pas la ou si l integration est desactivee.
    social::start();

    // Cache de metadonnees : un plafond, sinon il grossit indefiniment. Le
    // menage se fait au demarrage plutot qu'a chaque ecriture — parcourir
    // le dossier a chaque reponse mise en cache couterait plus cher que ce
    // qu'on economise.
    netcache::trim(64LL * 1024 * 1024);

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

// Phase 8, une fois par frame : préchauffage de l'instance sélectionnée,
// publication de l'instantané lu par l'API locale, et exécution des
// demandes qu'elle a déposées.
//
// Tout passe par ici parce que c'est le seul fil autorisé à toucher à
// l'état : le serveur HTTP vit sur le sien, et lire `settings.instances`
// pendant que l'interface le modifie serait un comportement indéfini.
namespace {

void phase8_pump(SDL_Window* window) {
    (void)window;
    const nlohmann::json* inst = selected_instance();

    // --- Préchauffage, sur changement de sélection seulement ---
    if (DataStore::settings.jvmPreload && inst && g.phase == Phase::Idle)
        jvmwarm::request(*inst);

    // --- Instantané pour l'API locale ---
    // Reconstruit au plus une fois par seconde : le sérialiser à 60 Hz
    // pour un client qui interroge toutes les cinq secondes serait du pur
    // gaspillage.
    if (localapi::running()) {
        static Uint32 lastPublish = 0;
        const Uint32 now = SDL_GetTicks();
        if (now - lastPublish > 1000) {
            lastPublish = now;
            const char* phase = g.phase == Phase::GameRunning ? "running"
                                : g.phase == Phase::Preparing ? "preparing"
                                : g.phase == Phase::Error     ? "error"
                                                              : "idle";
            nlohmann::json st{
                {"version", TL_VERSION_STRING},
                {"state", phase},
                {"selectedInstance", inst ? inst->value("Id", "") : ""},
                {"playerName", DataStore::settings.playerName},
            };
            nlohmann::json arr = nlohmann::json::array();
            for (const auto& e : inst_array()) {
                if (!e.is_object()) continue;
                // On ne publie QUE ces champs. Recopier l'instance entière
                // exposerait tout ce qu'on y ajoutera plus tard sans y
                // repenser, jetons compris.
                arr.push_back({
                    {"id", e.value("Id", "")},
                    {"name", e.value("Name", "")},
                    {"loader", e.value("Loader", "")},
                    {"mcVersion", e.value("McVersion", "")},
                    {"launches", e.value("Launches", 0)},
                    {"playSeconds", e.value("PlaySeconds", 0)},
                    {"lastPlayed", e.value("LastPlayed", "")},
                });
            }
            localapi::publish(st.dump(), arr.dump());
        }

        // --- Demandes de lancement déposées par l'API ---
        // Ces traces vont AUSSI dans launcher.log, et pas seulement dans
        // le panneau : une action déclenchée depuis l'extérieur doit
        // laisser quelque chose à relire. Personne ne regarde un panneau
        // au moment où un script agit.
        auto trace = [](const std::string& m) {
            push_log(m);
            log_line(m);
        };
        for (const auto& id : localapi::take_launch_requests()) {
            if (g.phase != Phase::Idle) {
                trace("API locale : lancement de " + id +
                      " ignoré (une partie est déjà en cours).");
                continue;
            }
            if (!find_instance(id)) {
                trace("API locale : instance inconnue (" + id + ").");
                continue;
            }
            trace("API locale : lancement de " + id + ".");
            g.selInstId = id;
            start_worker();
            break; // une seule par frame
        }
    }
}

} // namespace

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
        // Liaison Discord sans clic (repro du flux device).
        static bool autoDiscordLink = false;
        if (!autoDiscordLink) {
            autoDiscordLink = true;
            if (std::getenv("TL_AUTO_DISCORD_LINK")) social::begin_login();
        }
        if (const char* p = std::getenv("TL_AUTO_PAGE")) g.page = std::atoi(p);
        if (std::getenv("TL_AUTO_CTX")) g.ctxAuto = true;
        // TL_AUTO_COMPARE=1 : ouvre le comparateur sur la premiere
        // instance (capture de validation).
        if (std::getenv("TL_AUTO_COMPARE")) {
            for (const auto& e : inst_array())
                if (e.is_object() && !e.value("Id", "").empty()) {
                    compare_open(e.value("Id", ""));
                    break;
                }
        }
        // TL_AUTO_DETAIL=<onglet> : ouvre la page detail de la premiere
        // instance sur cet onglet (0 Infos, 1 Mods, 2 Mondes, 3 Journaux).
        // Sert aux captures de validation, comme les autres crochets.
        if (const char* dt = std::getenv("TL_AUTO_DETAIL")) {
            for (const auto& e : inst_array())
                if (e.is_object() && !e.value("Id", "").empty()) {
                    open_instance_detail(e.value("Id", ""), std::atoi(dt));
                    break;
                }
        }
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
    // Deux largeurs : complete (icone + libelle) et compacte (icone seule,
    // libelle en infobulle), comme le fait CurseForge. Le choix est retenu
    // dans la configuration, pas seulement pour la session.
    const bool compact = DataStore::settings.sidebarCompact;
    const float sideW = compact ? 56.0f : 170.0f;
    ImGui::BeginChild("##side", ImVec2(sideW, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    if (compact) {
        ImGui::TextUnformatted("TL");
    } else {
        ImGui::TextUnformatted("TEAM");
        ImGui::TextUnformatted("LAUNCHER");
    }
    ImGui::PopStyleColor();
    // Numero de version retire de la barre laterale : il n'apporte rien au
    // quotidien et occupait la place la plus visible de l'ecran. Il reste
    // affiche la ou on le cherche — page Aide, et Paramètres > Intégrations.
    ImGui::Spacing();
    ImGui::Spacing();

    auto nav = [&](icons::Id ic, const char* label, int page) {
        if (icons::nav_item(ic, tr(label), g.page == page, compact))
            g.page = page;
    };

    nav(icons::Id::Home, "Accueil", 0);
    nav(icons::Id::Instances, "Instances", 1);
    // Exploration porte l'index 9 : les index 0-8 etaient deja documentes
    // (TL_AUTO_PAGE) et sont laisses stables. L'ordre visuel est independant.
    nav(icons::Id::Explore, "Exploration", 9);
    nav(icons::Id::Files, "Explorateur", 10);
    nav(icons::Id::Map, "Édition de carte", 11);
    nav(icons::Id::City, "Ville OSM", 12);
    nav(icons::Id::ModDev, "Mods (dev)", 13);
    nav(icons::Id::Model, "Modèles 3D", 14);
    nav(icons::Id::Play, "Jouer", 2);
    nav(icons::Id::Servers, "Serveurs", 3);
    nav(icons::Id::Skins, "Skins", 4);
    nav(icons::Id::News, "Actualités", 5);
    nav(icons::Id::Bedrock, "Bedrock", 6);
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    nav(icons::Id::Download, "Téléchargements", 15);
    nav(icons::Id::Help, "Aide", 17);
    nav(icons::Id::Account, "Amis", 16);
    nav(icons::Id::Account, "Compte", 7);
    nav(icons::Id::Settings, "Paramètres", 8);

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
    case 15: downloads_page(); break;
    case 16: social_page(); break;
    case 17: help_page(); break;
    default: settings_page(); break;
    }
    ImGui::EndChild();

    // ---- Modales instance (contexte racine) ----
    instance_modals();
    instance_detail_modal();

    // ---- Notes de version (une seule fois apres une mise a jour) ----
    whatsnew_modal();

    // ---- Barrage de compatibilite des mods (phase 5) ----
    modcheck_gate_modal();
    compare_modal();

    // ---- Tâches de fond (panneau si >= 1 tâche) ----
    // Le minuteur de sauvegarde s'abstient tant qu'une partie tourne. On
    // derive l'etat de la phase courante a chaque frame plutot que de le
    // poser a chaque transition : impossible de le desynchroniser en
    // oubliant un chemin de sortie.
    backup::set_game_running(g.phase == Phase::GameRunning ||
                             g.phase == Phase::Preparing);

    // Le SDK social doit etre pompe une fois par frame pour delivrer ses
    // callbacks (connexion, amis). Sans effet s'il n'est pas la.
    social::pump();

    // ---- Phase 8 : préchauffage et pont de l'API locale ----
    phase8_pump(window);

    apptasks_frame();

    // Recherche globale : en dernier, pour passer au-dessus de tout le
    // reste, et hors de tout enfant pour que Ctrl+K marche depuis
    // n'importe quelle page.
    search_frame();

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
    // File de telechargements : annule ce qui court et JOINT les
    // travailleurs. Sans cela leurs threads survivent a la sortie de main()
    // et le processus se termine anormalement (code 9 observe).
    if (dbg) std::fprintf(stderr, "SH: downloads_stop\n");
    // Enregistrer AVANT d'annuler : shutdown() passe les elements en cours
    // a « annule », or on veut les retrouver a reprendre au prochain
    // demarrage, pas classes comme abandonnes par l'utilisateur.
    if (dbg) std::fprintf(stderr, "SH: backup_timer\n");
    backup::auto_stop();
    social::stop();
    downloads::save_state();
    downloads::shutdown();
    if (dbg) std::fprintf(stderr, "SH: downloads_stop done\n");
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
    // Analyse de compatibilite des mods : joint le worker.
    if (dbg) std::fprintf(stderr, "SH: modcheck_stop\n");
    modcheck_stop();
    modupdate_stop();
    compare_stop();
    // Préchauffage et API locale : annuler, joindre, fermer la socket.
    if (dbg) std::fprintf(stderr, "SH: jvmwarm_stop\n");
    jvmwarm::stop();
    if (dbg) std::fprintf(stderr, "SH: localapi_stop\n");
    localapi::stop();
    // Compteurs d'usage des clés : écrits une fois, pas à chaque appel.
    apikeys::flush();
    // Événements sortants : vider la file et joindre le fil.
    if (dbg) std::fprintf(stderr, "SH: events_stop\n");
    events::stop();
    // Page Aide : export de journaux et verification de mise a jour.
    if (dbg) std::fprintf(stderr, "SH: help_stop\n");
    help_stop();
    if (dbg) std::fprintf(stderr, "SH: help_stop done\n");
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

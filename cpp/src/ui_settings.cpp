#include "ui_internal.hpp"

#include "backup.hpp"
#include "downloads.hpp"
#include "netcache.hpp"
#include "fonts.hpp"
#include "game_launcher.hpp"

#include "maintenance.hpp"
#include "presence.hpp"
#include "shortcut.hpp"
#include "startup.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#endif

#include <functional>
#include <utility>

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

// ---------------------------------------------------------------------------
// Page Parametres — portage de SettingsPage (MiscPages.cs) : 4 onglets
// General / Apparence / Integrations / Avance.
//
// Le C# demandait « Redemarrer pour appliquer ? » a chaque changement de
// couleur, d'image ou de langue. En mode immediat tout est redessine a la
// frame suivante : on applique directement (aucun redemarrage).
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

// Tampons de saisie, initialises depuis les reglages a la premiere frame.
struct Buffers {
    char name[64] = "";
    char java[320] = "";
    char news[320] = "";
    char updateUrl[320] = "";
    char discordId[96] = "";
    char curseForge[160] = "";
    char webhook[320] = "";
    char helpDiscord[320] = "";
    char bg[16] = "";
    char card[16] = "";
    char accent[16] = "";
    bool init = false;
};
Buffers b;

void set(char* dst, size_t n, const std::string& v) {
    std::snprintf(dst, n, "%s", v.c_str());
}

void reload_buffers() {
    const auto& s = DataStore::settings;
    set(b.name, sizeof(b.name), s.playerName);
    set(b.java, sizeof(b.java), s.javaPath);
    set(b.news, sizeof(b.news), s.newsUrl);
    set(b.updateUrl, sizeof(b.updateUrl), s.updateUrl);
    set(b.discordId, sizeof(b.discordId), s.discordAppId);
    set(b.curseForge, sizeof(b.curseForge), s.curseForgeApiKey);
    set(b.webhook, sizeof(b.webhook), s.discordTelemetryWebhook);
    set(b.helpDiscord, sizeof(b.helpDiscord), s.helpDiscordUrl);
    set(b.bg, sizeof(b.bg), s.bgColor.empty() ? hex_of(kBg) : s.bgColor);
    set(b.card, sizeof(b.card), s.cardColor.empty() ? hex_of(kCard) : s.cardColor);
    set(b.accent, sizeof(b.accent),
        s.accentColor.empty() ? hex_of(kAccent) : s.accentColor);
    b.init = true;
}

// Libelle de section (SectionHeader C# : gras, TextDim, marge haute).
void section(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr(label));
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// Libelle de champ (SettingsLabel C#).
void field_label(const char* label) {
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr(label));
    ImGui::PopStyleColor();
}

std::optional<std::string> pick_image() {
#ifdef _WIN32
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
#else
    return pick_file_open("", "Images (*.png *.jpg *.jpeg *.bmp)",
                          "*.png *.jpg *.jpeg *.bmp");
#endif
}

// Champ couleur : saisie hexa + pastille d'apercu cliquable.
// Retourne true si la valeur a change (et est valide).
bool color_field(const char* label, char* buf, size_t n, std::string& target) {
    field_label(label);
    ImGui::SetNextItemWidth(120.0f);
    const std::string id = std::string("##col") + label;
    bool changed = false;
    if (ImGui::InputText(id.c_str(), buf, n)) {
        ImVec4 c;
        if (parse_hex_color(buf, c)) {
            target = trimmed(buf);
            changed = true;
        }
    }
    ImGui::SameLine();
    ImVec4 preview;
    if (parse_hex_color(buf, preview)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddRectFilled(
            p, ImVec2(p.x + 46.0f, p.y + h),
            ImGui::ColorConvertFloat4ToU32(preview), 4.0f);
        ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x + 46.0f, p.y + h),
                                            ImGui::ColorConvertFloat4ToU32(kBorder),
                                            4.0f);
        ImGui::Dummy(ImVec2(46.0f, h));
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(tr("format #rrggbb"));
        ImGui::PopStyleColor();
    }
    return changed;
}

// ---------------------------------------------------------------------------
// Onglets
// ---------------------------------------------------------------------------

void tab_general() {
    auto& s = DataStore::settings;

    field_label("Nom du joueur");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputText("##name", b.name, sizeof(b.name))) {
        s.playerName = trimmed(b.name);
        if (s.playerName.empty()) s.playerName = "Joueur";
        DataStore::save();
    }

    field_label("Chemin de Java (vide = détection automatique)");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputText("##java", b.java, sizeof(b.java))) {
        s.javaPath = trimmed(b.java);
        DataStore::save();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Parcourir..."))) {
        if (auto p = pick_javaw()) {
            set(b.java, sizeof(b.java), *p);
            s.javaPath = *p;
            DataStore::save();
        }
    }

    field_label("Mémoire maximale allouée à Minecraft (Go)");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::SliderInt("##ram", &s.maxRamGb, 1, 32, tr("%d Go", "%d GB")))
        DataStore::save();
    // Detection automatique : la regle est dans ideal_ram_gb (moitie de
    // la RAM physique, bornee 2-8 Go). On affiche la machine ET la valeur
    // conseillee plutot que de l'imposer : un joueur qui a choisi sa valeur
    // ne doit pas la voir changer dans son dos.
    {
        const long long totalMb = total_ram_mb();
        const int ideal = ideal_ram_gb(totalMb);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Détecter", "Detect"))) {
            s.maxRamGb = ideal;
            DataStore::save();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (totalMb > 0)
            ImGui::Text(tr("Machine : %lld Go de RAM — conseillé : %d Go",
                           "Machine: %lld GB of RAM - recommended: %d GB"),
                        static_cast<long long>((totalMb + 512) / 1024), ideal);
        else
            ImGui::Text(tr("RAM de la machine indéterminée — conseillé : %d Go",
                           "Machine RAM unknown - recommended: %d GB"),
                        ideal);
        ImGui::PopStyleColor();
    }

    field_label("Dossier des instances");
    {
        // Lecture seule (dirBox C# : ReadOnly).
        std::string dir = DataStore::instancesRoot().string();
        ImGui::SetNextItemWidth(420.0f);
        ImGui::BeginDisabled();
        ImGui::InputText("##instdir", dir.data(), dir.size() + 1,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Ouvrir", "Open")))
        open_in_explorer(DataStore::instancesRoot());

    section("ACTUALITÉS & LANGUE");
    field_label("URL des actualités (fichier JSON : title, date, tag, text)");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputText("##news", b.news, sizeof(b.news))) {
        s.newsUrl = trimmed(b.news);
        DataStore::save();
    }

    field_label("Langue");
    int langIdx = lang::is_en() ? 1 : 0;
    const char* langs[] = {"Français", "English"};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::Combo("##lang", &langIdx, langs, 2)) {
        // Pas de redemarrage : ImGui redessine tout a la frame suivante.
        lang::set_language(langIdx == 1 ? "en" : "fr");
        notify_toast(tr("Paramètres enregistrés."),
                     tr("Langue appliquée immédiatement.",
                        "Language applied immediately."));
    }

    // Format de date : independant de la langue. On peut vouloir lire le
    // launcher en francais et des dates ISO, ou l'inverse ; les lier
    // obligeait a changer de langue pour changer de format.
    field_label("Format des dates");
    {
        const char* const kFormats[] = {"dd/MM/yyyy", "MM/dd/yyyy",
                                        "yyyy-MM-dd"};
        int idx = 0;
        for (int i = 0; i < 3; ++i)
            if (s.dateFormat == kFormats[i]) idx = i;
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("##datefmt", &idx, kFormats, 3)) {
            s.dateFormat = kFormats[idx];
            DataStore::save();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text(tr("Aperçu : %s", "Preview: %s"),
                    format_date("2026-09-30").c_str());
        ImGui::PopStyleColor();
    }

    section("DÉMARRAGE ET FERMETURE");

    // Case a cocher lue a la SOURCE (registre / fichier autostart), pas
    // dans le reglage : un profil recopie ou un nettoyeur de demarrage
    // peut avoir retire l'entree, et afficher « activé » serait faux.
    {
        static bool known = false;
        static bool real = false;
        if (!known) {
            known = true;
            real = startup::autostart_enabled();
            // Reglage et realite d'accord : on recale sans rien ecrire au
            // systeme, l'utilisateur n'a rien demande.
            if (s.launchAtSystemStart != real) {
                s.launchAtSystemStart = real;
                DataStore::save();
            }
        }
        ImGui::BeginDisabled(!startup::autostart_supported());
        bool v = real;
        if (ImGui::Checkbox(tr("Lancer Team Launcher à l'ouverture de session",
                               "Start Team Launcher when I log in"),
                            &v)) {
            std::string err;
            if (startup::set_autostart(v, &err)) {
                real = v;
                s.launchAtSystemStart = v;
                DataStore::save();
            } else {
                notify_toast(tr("Démarrage automatique", "Automatic start"),
                             err);
            }
        }
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s",
                           tr("Le launcher démarre alors réduit : il ne "
                              "passe pas devant ce que vous faites.",
                              "The launcher then starts minimised: it does "
                              "not jump in front of what you are doing."));
        ImGui::PopStyleColor();
    }

    field_label("Bouton de fermeture de la fenêtre");
    {
        int idx = s.closeBehavior == "quit" ? 1 : 0;
        const char* names[] = {"Réduire la fenêtre", "Quitter le launcher"};
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("##close", &idx, names, 2)) {
            s.closeBehavior = idx == 1 ? "quit" : "minimize";
            DataStore::save();
        }
        if (idx == 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s",
                tr("Le launcher reste dans la barre des tâches. Pour le "
                   "fermer réellement : page Aide > Quitter le launcher.",
                   "The launcher stays in the taskbar. To really close it: "
                   "Help page > Quit the launcher."));
            ImGui::PopStyleColor();
        }
    }

    field_label("Instance sélectionnée au démarrage");
    {
        // Liste construite a chaque frame : elle doit suivre les creations
        // et suppressions d'instances sans qu'on ait a la rafraichir.
        std::vector<std::string> ids{"last", "none"};
        std::vector<std::string> labels{
            tr("La dernière jouée", "The last one played"),
            tr("Aucune", "None")};
        for (const auto& e : inst_array()) {
            if (!e.is_object()) continue;
            const std::string id = e.value("Id", "");
            if (id.empty()) continue;
            ids.push_back(id);
            labels.push_back(e.value("Name", id));
        }
        int idx = 0;
        for (size_t i = 0; i < ids.size(); ++i)
            if (ids[i] == s.startupGame) idx = static_cast<int>(i);
        std::vector<const char*> items;
        items.reserve(labels.size());
        for (const auto& l : labels) items.push_back(l.c_str());
        ImGui::SetNextItemWidth(320.0f);
        if (ImGui::Combo("##startgame", &idx, items.data(),
                         static_cast<int>(items.size()))) {
            s.startupGame = ids[static_cast<size_t>(idx)];
            DataStore::save();
        }
    }

    field_label("Salon d'entraide Discord (affiché dans la page Aide)");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputTextWithHint("##helpdisc", "https://discord.gg/...",
                                 b.helpDiscord, sizeof(b.helpDiscord))) {
        s.helpDiscordUrl = trimmed(b.helpDiscord);
        DataStore::save();
    }
}

void tab_appearance() {
    auto& s = DataStore::settings;

    // Le theme d'abord : les couleurs personnalisees ci-dessous se posent
    // PAR-DESSUS lui, donc l'ordre a l'ecran doit refleter l'ordre
    // d'application, sinon on ne comprend pas pourquoi changer de theme
    // « ne fait rien » quand une couleur a ete forcee.
    section("Thème");
    {
        struct Variant { const char* id; const char* fr; const char* en; };
        static const Variant kVariants[] = {
            {"classic", "Sombre (classique)", "Dark (classic)"},
            {"light", "Clair", "Light"},
        };
        std::string cur = tr(kVariants[0].fr, kVariants[0].en);
        for (const auto& v : kVariants)
            if (s.theme == v.id) cur = tr(v.fr, v.en);

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##theme", cur.c_str())) {
            for (const auto& v : kVariants)
                if (ImGui::Selectable(tr(v.fr, v.en), s.theme == v.id)) {
                    s.theme = v.id;
                    DataStore::save();
                    theme_reload();
                }
            ImGui::EndCombo();
        }

        bool cb = s.colorblind;
        if (ImGui::Checkbox(tr("Mode daltonisme", "Colorblind mode"), &cb)) {
            s.colorblind = cb;
            DataStore::save();
            theme_reload();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s",
            tr("Remplace le bleu et le rouge de l'interface par la palette "
               "Okabe-Ito, conçue pour rester distinguable dans les trois "
               "formes de daltonisme. Le rouge par défaut vire au brun pour "
               "une deutéranopie.",
               "Replaces the interface blue and red with the Okabe-Ito "
               "palette, designed to stay distinguishable in all three forms "
               "of color blindness. The default red turns brown under "
               "deuteranopia."));
        ImGui::PopStyleColor();

        // Si une couleur a ete forcee plus bas, le theme ne peut pas
        // s'appliquer entierement : mieux vaut le dire que de laisser
        // l'utilisateur croire a un bug.
        if (!s.bgColor.empty() || !s.cardColor.empty() || !s.accentColor.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
            ImGui::TextWrapped(
                "%s", tr("Des couleurs personnalisées sont définies plus bas : "
                         "elles s'appliquent par-dessus le thème. Utilise "
                         "« Couleurs par défaut » pour voir le thème seul.",
                         "Custom colors are set below: they apply on top of "
                         "the theme. Use \"Default colors\" to see the theme "
                         "on its own."));
            ImGui::PopStyleColor();
        }
    }

    section("Couleurs du launcher");
    bool changed = false;
    changed |= color_field("Fond", b.bg, sizeof(b.bg), s.bgColor);
    changed |= color_field("Cartes / panneaux", b.card, sizeof(b.card), s.cardColor);
    changed |= color_field("Accent (boutons)", b.accent, sizeof(b.accent),
                           s.accentColor);
    if (changed) {
        DataStore::save();
        theme_reload();
    }

    ImGui::Spacing();
    if (ImGui::Button(tr("Couleurs par défaut"), ImVec2(200, 34))) {
        s.bgColor.clear();
        s.cardColor.clear();
        s.accentColor.clear();
        DataStore::save();
        theme_reload();
        reload_buffers();
        notify_toast(tr("Apparence"), tr("Couleurs réinitialisées.",
                                         "Colors reset to default."));
    }

    // --- Police et échelle ---------------------------------------------
    // Le launcher se contentait de ProggyClean, la police bitmap de débogage
    // d'ImGui : floue une fois agrandie aux titres, et sans les glyphes de
    // ponctuation (« ... », tirets) qui s'affichaient donc en « ? ». On
    // charge maintenant une police du système — rien n'est redistribué, donc
    // aucune question de licence et pas un octet de plus dans le binaire.
    section("Police et taille du texte");
    {
        const auto& list = fonts::available();
        std::string current = tr("Automatique", "Automatic");
        for (const auto& c : list)
            if (c.id == s.uiFont) current = c.label;

        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::BeginCombo("##uifont", current.c_str())) {
            for (const auto& c : list)
                if (ImGui::Selectable(c.label.c_str(), c.id == s.uiFont)) {
                    s.uiFont = c.id;
                    DataStore::save();
                    fonts::request_rebuild();
                }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (list.size() <= 1)
            ImGui::TextUnformatted(
                tr("aucune police système trouvée : police de repli",
                   "no system font found: fallback font"));
        else
            ImGui::TextUnformatted(tr("police de l'interface", "interface font"));
        ImGui::PopStyleColor();

        field_label("Échelle du texte");
        float scale = static_cast<float>(s.fontScale);
        ImGui::SetNextItemWidth(260.0f);
        // Bornes 0,8–1,6 : en dessous le texte devient illisible, au-dessus
        // les libellés débordent de leurs boutons.
        if (ImGui::SliderFloat("##uiscale", &scale, 0.8f, 1.6f, "%.2fx")) {
            s.fontScale = scale;
        }
        // La reconstruction se fait au relâchement, pas à chaque pixel de
        // déplacement du curseur : reconstruire l'atlas coûte cher.
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            DataStore::save();
            fonts::request_rebuild();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Réinitialiser", "Reset"))) {
            s.fontScale = 1.0;
            s.uiFont = "auto";
            DataStore::save();
            fonts::request_rebuild();
        }
    }

    section("Téléchargements simultanés");
    {
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt("##maxdl", &s.maxDownloads, 1, 20, "%d")) {
            DataStore::save();
            downloads::set_limit(s.maxDownloads);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("Au-delà de quelques téléchargements en parallèle, le "
                     "débit total n'augmente plus et certains serveurs "
                     "limitent les connexions. 4 convient à la plupart des "
                     "connexions.",
                     "Past a few parallel downloads the total throughput no "
                     "longer increases and some servers throttle "
                     "connections. 4 suits most connections."));
        ImGui::PopStyleColor();
    }

    section("Barre latérale");
    {
        bool c = s.sidebarCompact;
        if (ImGui::Checkbox(tr("Icônes seules (libellé au survol)",
                               "Icons only (label on hover)"),
                            &c)) {
            s.sidebarCompact = c;
            DataStore::save();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("La barre passe de 170 à 56 pixels. Les icônes sont "
                     "dessinées en vectoriel, donc nettes à toutes les "
                     "échelles de texte.",
                     "The bar goes from 170 to 56 pixels. Icons are drawn as "
                     "vectors, so they stay sharp at any text scale."));
        ImGui::PopStyleColor();
    }

    section("Image de fond");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", s.backgroundImagePath.empty()
                                 ? tr("Aucune image de fond.")
                                 : (tr("Image actuelle : ", "Current image: ") +
                                    std::filesystem::path(s.backgroundImagePath)
                                        .filename()
                                        .string())
                                       .c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (ImGui::Button(tr("Choisir une image..."), ImVec2(200, 34))) {
        if (auto p = pick_image()) {
            s.backgroundImagePath = *p;
            DataStore::save();
            notify_toast(tr("Apparence"),
                         tr("Image enregistrée (rendu du fond : portage à venir).",
                            "Image saved (background rendering: port pending)."));
        }
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Retirer l'image"), ImVec2(160, 34))) {
        s.backgroundImagePath.clear();
        DataStore::save();
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", tr("Le chemin est conservé et partagé avec la v5 ; "
                                "l'affichage de l'image derrière l'interface "
                                "reste à porter.",
                                "The path is stored and shared with v5; drawing "
                                "the image behind the UI is not ported yet."));
    ImGui::PopStyleColor();
}

void tab_integrations();
void update_panel();

// Etat du panneau de mise a jour (onglet Integrations) : workers reseau hors
// UI, progression atomique lue chaque frame. Sans ImGui dans maintenance.* :
// tout le tissage est ici.
struct UpdState {
    std::mutex m;
    std::thread th;
    bool busy = false;
    std::atomic<bool> cancel{false};
    std::atomic<bool> downloading{false};
    std::atomic<long long> done{0};
    std::atomic<long long> total{-1};
    std::string status;
    std::optional<updates::Info> info;
    bool stagedReady = false;
    std::string stagedVer;
    bool stagedInit = false;
    int taskId = 0; // entree AppTasks du job en cours (sous m)
};
UpdState upd;

void upd_reap() {
    std::lock_guard<std::mutex> lk(upd.m);
    if (!upd.busy && upd.th.joinable()) upd.th.join();
}

void upd_set(const std::string& status, const std::optional<updates::Info>& info,
             bool stagedReady, const std::string& stagedVer) {
    std::lock_guard<std::mutex> lk(upd.m);
    upd.status = status;
    upd.info = info;
    upd.stagedReady = stagedReady;
    upd.stagedVer = stagedVer;
}

// Demarre un worker ET sa tache AppTasks associee. Le job recoit l'id de la
// tache et doit la cloturer lui-meme (apptasks_end) : le panneau reste juste
// si l'utilisateur quitte la page. false = deja en vol, aucune tache creee.
bool upd_start(const std::string& title, const std::string& status,
               const std::function<void(int)>& job) {
    upd_reap();
    int tid = 0;
    {
        std::lock_guard<std::mutex> lk(upd.m);
        if (upd.busy) return false;
        upd.busy = true;
        upd.cancel.store(false);
        upd.done.store(0);
        upd.total.store(-1);
        // Verrous : upd.m -> verrous du registre (aucun chemin inverse).
        tid = upd.taskId = apptasks_begin(title, status, &upd.cancel);
    }
    upd.th = std::thread([job, tid] {
        job(tid);
        upd.downloading.store(false);
        std::lock_guard<std::mutex> lk(upd.m);
        upd.busy = false;
        upd.taskId = 0;
    });
    return true;
}

// Portage du bloc « MISES À JOUR AUTOMATIQUES » de SettingsPage.cs (bouton
// « Vérifier maintenant », confirmation, telechargement puis installation).
// Velopack appliquait en cours d'execution ; ici le paquet est stage puis le
// script apply-update.bat deploie APRES la sortie (bouton Installer).
void update_panel() {
    upd_reap();

    // Au premier affichage (donc a chaque demarrage si l'onglet est ouvert,
    // et de toute facon sans reseau) : un marqueur pending.json survivant
    // propose directement « Installer et redémarrer ».
    if (!upd.stagedInit) {
        upd.stagedInit = true;
        if (auto s = updates::staged()) {
            std::lock_guard<std::mutex> lk(upd.m);
            upd.stagedReady = true;
            upd.stagedVer = s->info.version;
            upd.info = s->info;
            upd.status = "Mise à jour v" + s->info.version +
                         " prête (téléchargée précédemment).";
        }
    }

    bool busy, downloading, stagedReady;
    std::string status, stagedVer;
    std::optional<updates::Info> info;
    int taskId;
    {
        std::lock_guard<std::mutex> lk(upd.m);
        busy = upd.busy;
        stagedReady = upd.stagedReady;
        status = upd.status;
        stagedVer = upd.stagedVer;
        info = upd.info;
        taskId = upd.taskId;
    }
    downloading = upd.downloading.load();

    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr("Vérifier les mises à jour"), ImVec2(260, 34))) {
        // Textes resolus ici (fil UI) : ni tr() ni upd sans verrou dans le job.
        const std::string checking = tr("Vérification...");
        const std::string cancelledTxt = tr("Vérification annulée.",
                                            "Check cancelled.");
        const std::string upToDate = std::string("Tu es déjà à la dernière version (v") +
                                     updates::current_version() + ").";
        bool stagedSnap;
        std::string stagedVerSnap;
        {
            std::lock_guard<std::mutex> lk(upd.m);
            stagedSnap = upd.stagedReady;
            stagedVerSnap = upd.stagedVer;
        }
        upd_start(tr("Vérification des mises à jour", "Update check"),
                  checking,
                  [checking, cancelledTxt, upToDate, stagedSnap,
                   stagedVerSnap](int tid) {
            upd_set(checking, std::nullopt, stagedSnap, stagedVerSnap);
            std::string err;
            auto found = updates::check(&err);
            // updates::check() n'est pas annulable : si l'annulation est
            // arrivee entre-temps, on ne publie pas le resultat et la tache
            // reste « Annulee ».
            if (upd.cancel.load()) {
                (void)tl::tasks::cancel(tid);
                upd_set(cancelledTxt, std::nullopt, stagedSnap, stagedVerSnap);
                apptasks_end(tid);
                return;
            }
            if (found) {
                const std::string msg =
                    "Nouvelle version disponible : v" + found->version +
                    " (tu es en v" + updates::current_version() + ").";
                upd_set(msg, found, stagedSnap, stagedVerSnap);
                tl::tasks::update(tid, msg);
                apptasks_end(tid);
            } else if (!err.empty()) {
                upd_set(err, std::nullopt, stagedSnap, stagedVerSnap);
                apptasks_end(tid, err);
            } else {
                upd_set(upToDate, std::nullopt, stagedSnap, stagedVerSnap);
                tl::tasks::update(tid, upToDate);
                apptasks_end(tid);
            }
        });
    }
    ImGui::EndDisabled();

    if (busy && downloading) {
        const long long d = upd.done.load(), t = upd.total.load();
        if (t > 0) {
            const float f = static_cast<float>(
                static_cast<double>(d) / static_cast<double>(t));
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%lld / %lld Ko", d / 1024,
                          t / 1024);
            ImGui::ProgressBar(f, ImVec2(420, 0), overlay);
        } else {
            ImGui::ProgressBar(-1.0f, ImVec2(420, 0), tr("Téléchargement..."));
        }
        if (ImGui::Button(tr("Annuler"), ImVec2(160, 30))) {
            upd.cancel.store(true);
            // Meme annulation que celle du panneau de taches.
            if (taskId) (void)tl::tasks::cancel(taskId);
        }
    } else if (busy) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Vérification..."));
        ImGui::PopStyleColor();
    }

    // Release trouvee et rien de stage : changelog + telechargement (equivaut
    // au dialogue « Mettre à jour maintenant ? » du C#).
    if (!busy && !stagedReady && info && !info->notes.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%.800s", info->notes.c_str());
        ImGui::PopStyleColor();
    }
    if (!busy && !stagedReady && info) {
        ImGui::Spacing();
        if (!info->assetUrl.empty()) {
            if (accent_button(tr("Télécharger la mise à jour"), ImVec2(280, 34))) {
                updates::Info copy = *info;
                const std::string downloadingTxt = tr("Téléchargement...");
                const std::string cancelledTxt = tr("Téléchargement annulé.");
                const std::string failTxt =
                    tr("Échec du téléchargement.", "Download failed.");
                const std::string title =
                    std::string(tr("Mise à jour", "Update")) + " v" + copy.version;
                upd.downloading.store(true);
                upd_start(title, downloadingTxt,
                          [copy, downloadingTxt, cancelledTxt, failTxt](int tid) {
                    upd_set(downloadingTxt, copy, false, "");
                    std::string err;
                    // Progression : atomique locale (UI) + miroir 0..1 pour
                    // le panneau de taches.
                    auto prog = [tid, downloadingTxt](long long d, long long t) {
                        upd.done.store(d);
                        upd.total.store(t);
                        if (t > 0)
                            tl::tasks::update(
                                tid, downloadingTxt,
                                static_cast<double>(d) / static_cast<double>(t));
                    };
                    if (updates::download_update(copy, prog, &upd.cancel, &err)) {
                        const std::string msg =
                            "Mise à jour v" + copy.version +
                            " téléchargée : installe-la quand tu veux.";
                        upd_set(msg, copy, true, copy.version);
                        tl::tasks::update(tid, msg);
                        apptasks_end(tid);
                    } else if (upd.cancel.load()) {
                        (void)tl::tasks::cancel(tid);
                        upd_set(cancelledTxt, copy, false, "");
                        apptasks_end(tid);
                    } else {
                        upd_set(err, copy, false, "");
                        apptasks_end(tid, err.empty() ? failTxt : err);
                    }
                });
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s",
                tr("Cette release ne contient pas de paquet Windows (.zip).",
                   "This release has no Windows (.zip) package."));
            ImGui::PopStyleColor();
        }
        if (!info->url.empty()) {
            if (ImGui::Button(tr("Ouvrir la page de la version"), ImVec2(280, 34)))
                open_url(info->url);
        }
    }

    // Staged pret : installation differee + redemarrage propose.
    if (!busy && stagedReady) {
        ImGui::Spacing();
        const std::string installLbl =
            tr(std::string("Installer v") + stagedVer + " et redémarrer");
        if (accent_button(installLbl.c_str(), ImVec2(320, 36))) {
            std::string err;
            if (updates::install_staged_and_restart(&err)) {
                notify_toast(tr("Mise à jour"),
                             tr("Installation au redémarrage : le launcher "
                                "va se fermer.",
                                "Installing on restart: the launcher will now "
                                "close."));
                SDL_Event ev{};
                ev.type = SDL_QUIT;
                SDL_PushEvent(&ev);
            } else {
                notify_toast(tr("Mise à jour"), err);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Supprimer le paquet"), ImVec2(180, 36))) {
            std::string err;
            if (updates::clear_staged(&err)) {
                upd_set(tr("Paquet de mise à jour supprimé."), std::nullopt,
                        false, "");
            } else {
                notify_toast(tr("Mise à jour"), err);
            }
        }
    }

    if (!status.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
    }
}

void tab_integrations() {
    auto& s = DataStore::settings;

    section("Rich Presence Discord");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", tr("Affiche sur ton profil Discord l'instance en "
                                "cours, la version et le temps de jeu (Discord "
                                "doit être ouvert).",
                                "Shows the current instance, version and playtime "
                                "on your Discord profile (Discord must be open)."));
    ImGui::PopStyleColor();
    if (ImGui::Checkbox(tr("Activer la Rich Presence Discord"), &s.discordEnabled))
        DataStore::save();
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputTextWithHint(
            "##dcid", tr("ID d'application Discord (discord.com/developers/applications)"),
            b.discordId, sizeof(b.discordId))) {
        s.discordAppId = trimmed(b.discordId);
        DataStore::save();
    }
    if (ImGui::Button(tr("Appliquer"), ImVec2(160, 34))) {
        presence::reload();
        notify_toast(tr("Discord"),
                     presence::enabled()
                         ? tr("Rich Presence activée - ouvre Discord pour voir "
                              "ton statut.",
                              "Rich Presence enabled - open Discord to see your "
                              "status.")
                         : tr("Rich Presence désactivée.",
                              "Rich Presence disabled."));
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text,
                          presence::connected() ? hex(0x50c878) : kDim);
    ImGui::TextUnformatted(presence::connected()
                               ? tr("Connecté à Discord", "Connected to Discord")
                               : tr("Non connecté", "Not connected"));
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", tr("Le logo doit être téléversé sur "
                                "discord.com/developers/applications sous le nom "
                                "exact « logo » (Rich Presence > Art Assets).",
                                "The logo must be uploaded to "
                                "discord.com/developers/applications under the "
                                "exact name \"logo\" (Rich Presence > Art Assets)."));
    ImGui::PopStyleColor();

    section("MISES À JOUR AUTOMATIQUES");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputTextWithHint(
            "##upd", tr("URL du flux de mises à jour (Velopack, optionnel)"),
            b.updateUrl, sizeof(b.updateUrl))) {
        s.updateUrl = trimmed(b.updateUrl);
        DataStore::save();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("%s%s", tr("Version installée : "), TL_VERSION_STRING);
    ImGui::PopStyleColor();

    // Canal. « /releases/latest » ne renvoie jamais de preversion : passer
    // en bêta change de point d'entree, pas seulement de filtre.
    field_label("Canal de mise à jour");
    {
        int ch = s.updateChannel == "beta" ? 1 : 0;
        const char* names[] = {"Stable", "Bêta"};
        ImGui::SetNextItemWidth(200.0f);
        if (ImGui::Combo("##chan", &ch, names, 2)) {
            s.updateChannel = ch == 1 ? "beta" : "stable";
            DataStore::save();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s",
            ch == 1
                ? tr("Les préversions sont proposées dès leur publication. "
                     "Elles ne sont pas relues : attendez-vous à des "
                     "régressions, et gardez une sauvegarde de vos mondes.",
                     "Pre-releases are offered as soon as they are "
                     "published. They are not reviewed: expect "
                     "regressions, and keep a backup of your worlds.")
                : tr("Seules les versions publiées comme définitives sont "
                     "proposées.",
                     "Only releases published as final are offered."));
        ImGui::PopStyleColor();
    }

    field_label("Vérifier les mises à jour");
    ImGui::SetNextItemWidth(320.0f);
    if (ImGui::SliderInt("##updfreq", &s.updateFreqHours, 0, 168,
                         s.updateFreqHours == 0
                             ? tr("seulement à la demande", "on request only")
                             : tr("toutes les %d h", "every %d h")))
        DataStore::save();

    // Portage UpdateService/UpdateChecker (sans Velopack) : verification,
    // telechargement avec progression, installation differee au redemarrage.
    update_panel();

    section("CURSEFORGE");
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputTextWithHint(
            "##cf", tr("Clé API CurseForge (console.curseforge.com - gratuite)"),
            b.curseForge, sizeof(b.curseForge), ImGuiInputTextFlags_Password)) {
        s.curseForgeApiKey = trimmed(b.curseForge);
        DataStore::save();
    }

    section("TÉLÉMÉTRIE & LOGS DISTANTS");
    if (ImGui::Checkbox(
            tr("Envoyer les rapports de crash et stats d'utilisation vers Discord"),
            &s.telemetryEnabled))
        DataStore::save();
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputTextWithHint("##hook", tr("URL du webhook Discord"), b.webhook,
                                 sizeof(b.webhook), ImGuiInputTextFlags_Password)) {
        s.discordTelemetryWebhook = trimmed(b.webhook);
        DataStore::save();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s", tr("Les rapports incluent : crashs Minecraft, crashs du launcher, "
                 "stats de lancement.\nAucune donnée personnelle n'est envoyée "
                 "(pas de pseudo, pas de mots de passe).",
                 "Reports include: Minecraft crashes, launcher crashes, launch "
                 "stats.\nNo personal data is sent (no username, no passwords)."));
    ImGui::PopStyleColor();
}

// --- Maintenance : diagnostic / nettoyage / mise a jour (worker unique) -----

struct MaintState {
    std::mutex m;
    std::thread th;
    bool running = false;
    bool done = false;
    std::atomic<bool> cancel{false}; // annulation du diagnostic (panneau)
    int taskId = 0; // entree AppTasks du job en cours (sous m)
    // resultats
    std::vector<health::Check> checks;
    bool haveChecks = false;
    std::string updateMsg;
    std::string updateUrl;
};
MaintState maint;

// Joint le thread precedent une fois son travail termine.
void maint_reap() {
    std::lock_guard<std::mutex> lk(maint.m);
    if (maint.done && maint.th.joinable()) {
        maint.th.join();
        maint.done = false;
    }
}

// Idem upd_start : worker + tache AppTasks liee a maint.cancel.
bool maint_start(const std::string& title, const std::string& status,
                 const std::function<void(int)>& job) {
    maint_reap();
    int tid = 0;
    {
        std::lock_guard<std::mutex> lk(maint.m);
        if (maint.running) return false;
        if (maint.th.joinable()) maint.th.join();
        maint.running = true;
        maint.cancel.store(false);
        tid = maint.taskId = apptasks_begin(title, status, &maint.cancel);
    }
    maint.th = std::thread([job, tid] {
        job(tid);
        std::lock_guard<std::mutex> lk(maint.m);
        maint.running = false;
        maint.done = true;
        maint.taskId = 0;
    });
    return true;
}

void tab_advanced() {
    auto& s = DataStore::settings;
    maint_reap();

    // --- Sauvegardes automatiques ---
    section("Sauvegarde automatique des mondes");
    {
        ImGui::SetNextItemWidth(260.0f);
        int h = s.backupAutoHours;
        if (ImGui::SliderInt("##bkhours", &h, 0, 48,
                             h == 0 ? tr("désactivée", "off")
                                    : tr("toutes les %d h", "every %d h"))) {
            s.backupAutoHours = h;
            DataStore::save();
            // Le minuteur lit le reglage a chaque reveil : rien a relancer.
        }
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt(tr("Archives conservées", "Archives kept"),
                             &s.backupKeep, 1, 30, "%d"))
            DataStore::save();
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::SliderInt(tr("Espace alloué", "Allocated space"),
                             &s.backupSpaceMb, 0, 20480,
                             s.backupSpaceMb == 0
                                 ? tr("illimité", "unlimited")
                                 : "%d Mo"))
            DataStore::save();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s",
            tr("Zippe le dossier des mondes de chaque instance. Jamais "
               "pendant une partie : archiver un monde en cours d'écriture "
               "donnerait une sauvegarde inutilisable. Les plus anciennes "
               "sont supprimées au-delà du nombre ou de l'espace, mais la "
               "plus récente est toujours gardée.",
               "Zips each instance's worlds folder. Never while playing: "
               "archiving a world being written would produce an unusable "
               "backup. The oldest are removed beyond the count or the "
               "space, but the most recent one is always kept."));
        ImGui::PopStyleColor();
    }

    // --- Cache des metadonnees ---
    // La taille se lit en parcourant le dossier : trop cher a faire a
    // chaque frame. On la relit sur demande, et apres un vidage.
    section("Cache des métadonnées");
    {
        static long long cacheSz = -1;
        static bool asked = false;
        if (!asked) {
            asked = true;
            cacheSz = netcache::size_bytes();
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s",
            tr("Les fiches de mods et les résultats de recherche sont gardés "
               "15 minutes sur le disque : les mêmes pages ne sont pas "
               "redemandées sans arrêt, et si le réseau tombe le launcher "
               "affiche la dernière version connue au lieu d'une page vide. "
               "Plafonné à 64 Mo, nettoyé au démarrage.",
               "Mod pages and search results are kept on disk for 15 "
               "minutes: the same pages are not re-requested constantly, and "
               "if the network drops the launcher shows the last known "
               "version instead of an empty page. Capped at 64 MB, cleaned "
               "at startup."));
        ImGui::PopStyleColor();
        // Unite adaptee : un cache de 27 Ko affiche « 0,0 Mo », ce qui donne
        // l'impression qu'il ne fonctionne pas.
        const long long sz = cacheSz < 0 ? 0 : cacheSz;
        if (sz < 1024 * 1024)
            ImGui::Text(tr("Taille actuelle : %lld Ko", "Current size: %lld KB"),
                        (sz + 512) / 1024);
        else
            ImGui::Text(tr("Taille actuelle : %.1f Mo", "Current size: %.1f MB"),
                        sz / (1024.0 * 1024.0));
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Actualiser", "Refresh")))
            cacheSz = netcache::size_bytes();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Vider le cache", "Clear cache"))) {
            netcache::clear();
            cacheSz = netcache::size_bytes();
            notify_toast(tr("Cache"),
                         tr("Cache des métadonnées vidé.",
                            "Metadata cache cleared."));
        }
    }

    if (ImGui::Checkbox(tr("Compter les FPS"), &s.fpsCounterEnabled))
        DataStore::save();
    field_label("Quand la partie démarre");
    {
        const char* const kModes[] = {"nothing", "minimize", "quit"};
        const char* names[] = {tr("Ne rien faire", "Do nothing"),
                               tr("Réduire la fenêtre", "Minimise the window"),
                               tr("Quitter le launcher", "Quit the launcher")};
        int idx = 1;
        for (int i = 0; i < 3; ++i)
            if (s.onGameLaunch == kModes[i]) idx = i;
        ImGui::SetNextItemWidth(260.0f);
        if (ImGui::Combo("##ongamelaunch", &idx, names, 3)) {
            s.onGameLaunch = kModes[idx];
            // Miroir pour la v5 Avalonia, qui partage ce fichier.
            s.minimizeOnLaunch = s.onGameLaunch == "minimize";
            DataStore::save();
        }
        if (s.onGameLaunch == "quit") {
            // Dire le prix AVANT que l'utilisateur s'aperçoive que son
            // temps de jeu n'augmente plus. Le launcher ne verra pas la
            // fin de la partie : tout ce qui se fait à ce moment-là est
            // perdu, et ce n'est pas rattrapable.
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s",
                tr("Minecraft continue de tourner — le launcher ne le tue "
                   "pas. Le temps de jeu est rattrapé au prochain "
                   "démarrage, estimé d'après les fichiers écrits par le "
                   "jeu : comptez une minute près.",
                   "Minecraft keeps running - the launcher does not kill "
                   "it. Playtime is recovered on the next start, estimated "
                   "from the files the game wrote: expect about a minute's "
                   "accuracy."));
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE0A030));
            ImGui::TextWrapped(
                "%s",
                tr("Restent perdues, elles : l'analyse de crash de cette "
                   "partie et la sauvegarde automatique des mondes après "
                   "y avoir joué. La présence Discord s'arrête aussi.",
                   "Still lost: the crash analysis for that session and "
                   "the automatic world backup afterwards. Discord "
                   "presence also stops."));
            ImGui::PopStyleColor();
        }
    }

    // ---- Phase 8 : plugins, API locale, préchauffage ----
    jvmwarm_panel();
    localapi_panel();
    plugins_panel();

    section("Maintenance");
    if (ImGui::Button(tr("Ouvrir le dossier de données"), ImVec2(260, 34)))
        open_in_explorer(DataStore::dir());
    ImGui::SameLine();
    if (ImGui::Button(tr("Ouvrir launcher.log"), ImVec2(200, 34)))
        open_in_explorer(DataStore::dir() / "launcher.log");
    ImGui::SameLine();
    // Le C# ne creait le raccourci qu'une fois, sans moyen de le refaire.
    if (ImGui::Button(tr("Raccourci sur le bureau"), ImVec2(240, 34))) {
        const bool done = ensure_desktop_shortcut(/*force=*/true);
        notify_toast(tr("Raccourci"),
                     done ? tr("Raccourci créé sur le bureau.",
                               "Shortcut created on the desktop.")
                          : tr("Création impossible (voir launcher.log).",
                               "Could not create it (see launcher.log)."));
    }

    bool busy;
    {
        std::lock_guard<std::mutex> lk(maint.m);
        busy = maint.running;
    }

    // TL_AUTO_DIAG=1 : lance le diagnostic sans clic (test).
    static bool autoDiag = std::getenv("TL_AUTO_DIAG") != nullptr;

    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(tr("Diagnostic du système"), ImVec2(260, 34)) ||
        std::exchange(autoDiag, false)) {
        {
            std::lock_guard<std::mutex> lk(maint.m);
            maint.haveChecks = false;
            maint.updateMsg.clear();
        }
        const std::string cancelledTxt =
            tr("Diagnostic annulé.", "Diagnostic cancelled.");
        maint_start(tr("Diagnostic du système", "System diagnostic"),
                    tr("Analyse en cours...", "Analyzing..."),
                    [cancelledTxt](int tid) {
            auto r = health::run_all(&maint.cancel); // annulable
            const bool wasCancelled = maint.cancel.load();
            if (wasCancelled) (void)tl::tasks::cancel(tid);
            {
                std::lock_guard<std::mutex> lk(maint.m);
                if (wasCancelled) {
                    maint.updateMsg = cancelledTxt;
                } else {
                    maint.checks = std::move(r);
                    maint.haveChecks = true;
                }
            }
            apptasks_end(tid);
        });
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Libérer de l'espace (cache)"), ImVec2(260, 34))) {
        const auto r = cleanup::run();
        char msg[160];
        std::snprintf(msg, sizeof(msg),
                      tr("%d fichier(s) supprimé(s), %.2f Mo libérés.",
                         "%d file(s) deleted, %.2f MB freed."),
                      r.files, r.mb);
        notify_toast(tr("Maintenance"), msg);
    }
    // 3e bouton sur sa propre ligne : 3 x 260 px depassent en fenetre etroite.
    if (ImGui::Button(tr("Vérifier les mises à jour"), ImVec2(260, 34))) {
        {
            std::lock_guard<std::mutex> lk(maint.m);
            maint.haveChecks = false;
            maint.updateMsg.clear();
            maint.updateUrl.clear();
        }
        const std::string cancelledTxt =
            tr("Vérification annulée.", "Check cancelled.");
        maint_start(tr("Vérification des mises à jour", "Update check"),
                    tr("Vérification...", "Checking..."),
                    [cancelledTxt](int tid) {
            std::string err;
            auto info = updates::check(&err);
            // Verification non annulable : annulation arrivee entre-temps ->
            // on ignore le resultat, la tache reste « Annulee ».
            if (maint.cancel.load()) {
                (void)tl::tasks::cancel(tid);
                {
                    std::lock_guard<std::mutex> lk(maint.m);
                    maint.updateMsg = cancelledTxt;
                }
                apptasks_end(tid);
                return;
            }
            std::string msg, url;
            if (info) {
                msg = "Nouvelle version disponible : v" + info->version +
                      " (tu es en v" + updates::current_version() + ").";
                url = info->url;
            } else if (!err.empty()) {
                msg = err;
            } else {
                msg = std::string("Tu es déjà à la dernière version (v") +
                      updates::current_version() + ").";
            }
            {
                std::lock_guard<std::mutex> lk(maint.m);
                maint.updateMsg = msg;
                maint.updateUrl = url;
            }
            if (!err.empty() && !info) {
                apptasks_end(tid, err);
            } else {
                tl::tasks::update(tid, msg);
                apptasks_end(tid);
            }
        });
    }
    ImGui::EndDisabled();

    if (busy) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Vérification..."));
        ImGui::PopStyleColor();
    }

    // --- Resultats ---
    std::lock_guard<std::mutex> lk(maint.m);
    if (maint.haveChecks && !maint.checks.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::BeginChild("##diag",
                          ImVec2(0, 24.0f + maint.checks.size() * 24.0f),
                          ImGuiChildFlags_Borders);
        for (const auto& c : maint.checks) {
            ImGui::PushStyleColor(ImGuiCol_Text, c.ok ? hex(0x50c878) : kDanger);
            ImGui::Text("%s %s", c.ok ? "OK " : "KO ", c.name.c_str());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            // « — » (U+2014) est hors de GetGlyphRangesDefault() : on garde « · ».
            ImGui::Text("· %s", c.detail.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    if (!maint.updateMsg.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", maint.updateMsg.c_str());
        ImGui::PopStyleColor();
        if (!maint.updateUrl.empty()) {
            if (accent_button(tr("Ouvrir la page de la version"), ImVec2(280, 34)))
                open_url(maint.updateUrl);
        }
    }
}

} // namespace

// Joint le worker de maintenance (shutdown). Le thread est sorti sous verrou
// puis joint hors verrou : son bloc final reprend `maint.m`.
void settings_stop() {
    upd.cancel.store(true);
    maint.cancel.store(true);
    upd_reap();
    {
        std::lock_guard<std::mutex> lk(upd.m);
        if (upd.th.joinable() && !upd.busy) upd.th.join();
    }
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(maint.m);
        th = std::move(maint.th);
    }
    if (th.joinable()) th.join();
    {
        std::lock_guard<std::mutex> lk(upd.m);
        if (upd.th.joinable()) upd.th.join();
    }
}

namespace {

} // namespace

void settings_page() {
    if (!b.init) reload_buffers();

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Paramètres"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    // TL_AUTO_TAB : selection d'onglet pour les tests (0..3), lue une fois.
    static int autoTab = -2;
    if (autoTab == -2) {
        const char* v = std::getenv("TL_AUTO_TAB");
        autoTab = v ? std::atoi(v) : -1;
    }
    auto forced = [&](int i) {
        // Demande venue d'ailleurs (page Aide) : elle prime, et se consomme.
        if (g.settingsTab == i) {
            g.settingsTab = -1;
            return ImGuiTabItemFlags_SetSelected;
        }
        if (autoTab != i) return ImGuiTabItemFlags_None;
        autoTab = -1;
        return ImGuiTabItemFlags_SetSelected;
    };

    if (ImGui::BeginTabBar("##settabs")) {
        ImGui::PushStyleColor(ImGuiCol_Tab, kCard);
        ImGui::PushStyleColor(ImGuiCol_TabActive, kAccent);
        if (ImGui::BeginTabItem(tr("Général"), nullptr, forced(0))) {
            ImGui::BeginChild("##tgen");
            tab_general();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Apparence"), nullptr, forced(1))) {
            ImGui::BeginChild("##tapp");
            tab_appearance();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Intégrations"), nullptr, forced(2))) {
            ImGui::BeginChild("##tint");
            tab_integrations();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(tr("Avancé"), nullptr, forced(3))) {
            ImGui::BeginChild("##tadv");
            tab_advanced();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::PopStyleColor(2);
        ImGui::EndTabBar();
    }
}

} // namespace tl::ui

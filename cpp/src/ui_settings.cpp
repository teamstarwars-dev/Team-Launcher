#include "ui_internal.hpp"

#include "maintenance.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>

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
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Images\0*.png;*.jpg;*.jpeg;*.bmp\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
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
}

void tab_appearance() {
    auto& s = DataStore::settings;

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
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", tr("Le service de présence (PresenceService) n'est pas "
                                "encore porté : le réglage est conservé.",
                                "The presence service (PresenceService) is not "
                                "ported yet: the setting is stored."));
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

void maint_start(std::function<void()> job) {
    maint_reap();
    std::lock_guard<std::mutex> lk(maint.m);
    if (maint.running) return;
    if (maint.th.joinable()) maint.th.join();
    maint.running = true;
    maint.th = std::thread([job = std::move(job)] {
        job();
        std::lock_guard<std::mutex> lk(maint.m);
        maint.running = false;
        maint.done = true;
    });
}

void tab_advanced() {
    auto& s = DataStore::settings;
    maint_reap();

    if (ImGui::Checkbox(tr("Compter les FPS"), &s.fpsCounterEnabled))
        DataStore::save();
    if (ImGui::Checkbox(tr("Minimiser le launcher au lancement"), &s.minimizeOnLaunch))
        DataStore::save();

    section("Maintenance");
    if (ImGui::Button(tr("Ouvrir le dossier de données"), ImVec2(260, 34)))
        open_in_explorer(DataStore::dir());
    ImGui::SameLine();
    if (ImGui::Button(tr("Ouvrir launcher.log"), ImVec2(200, 34)))
        open_in_explorer(DataStore::dir() / "launcher.log");

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
        maint_start([] {
            auto r = health::run_all();
            std::lock_guard<std::mutex> lk(maint.m);
            maint.checks = std::move(r);
            maint.haveChecks = true;
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
        maint_start([] {
            std::string err;
            auto info = updates::check(&err);
            std::lock_guard<std::mutex> lk(maint.m);
            if (info) {
                maint.updateMsg = "Nouvelle version disponible : v" + info->version +
                                  " (tu es en v" + updates::current_version() + ").";
                maint.updateUrl = info->url;
            } else if (!err.empty()) {
                maint.updateMsg = err;
            } else {
                maint.updateMsg =
                    std::string("Tu es déjà à la dernière version (v") +
                    updates::current_version() + ").";
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
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(maint.m);
        th = std::move(maint.th);
    }
    if (th.joinable()) th.join();
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

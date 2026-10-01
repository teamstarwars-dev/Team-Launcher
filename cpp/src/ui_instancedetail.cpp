// Portage fidele de InstanceDetailWindow.cs (TeamLauncher.Avalonia) :
// banniere + onglets Infos / Mods / Mondes / Journaux, actions contextuelles.
// Sections C# non portees (service reseau / lecture binaire non portes en C++)
// signalees "non porte" dans l'UI plutot qu'inventees :
//   - onglets Shaders / Resource Packs / Configs / Screenshots (boutons dossier)
//   - installation auto de WorldEdit (telechargement reseau)
//   - import CurseForge + lecture level.dat (WorldTools)
//   - ajout de mod par selecteur (copie manuelle via dossier ouvert)
// Helpers purs (sans ImGui) dans tl::ui::detail, testes par
// tests/test_instance_detail.cpp via include direct avec TL_DETAIL_LOGIC_ONLY.

#ifdef TL_DETAIL_LOGIC_ONLY
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#else
#include "ui_internal.hpp"

#include "presets.hpp"

#include "backup.hpp"
#include "util_zip.hpp" // zip_top_level_dirs : apercu d'une sauvegarde

#include <ctime>
#include <map>
#include <utility>

#include <fstream>
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace tl::ui::detail {

// C# LoadMods : f.EndsWith(".disabled", OrdinalIgnoreCase)
bool mod_is_disabled(const std::string& fileName) {
    static const char kSuf[] = ".disabled";
    static const size_t kLen = sizeof(kSuf) - 1;
    if (fileName.size() <= kLen) return false;
    const std::string tail = fileName.substr(fileName.size() - kLen);
    for (size_t i = 0; i < kLen; ++i)
        if ((char)tolower((unsigned char)tail[i]) != kSuf[i]) return false;
    return true;
}

// C# LoadMods : fi.Name[..^".disabled".Length]
std::string mod_base_name(const std::string& fileName) {
    if (mod_is_disabled(fileName))
        return fileName.substr(0, fileName.size() - 9);
    return fileName;
}

// C# LoadMods/LoadWorlds/LoadFiles : "X,X Mo" si > 1 Mio sinon "X[,X] Ko"
// (format C# "0.#" : une decimale max, virgule francaise).
std::string format_size_fr(long long bytes) {
    const double mib = (double)bytes / 1048576.0;
    char buf[64];
    if (mib > 1.0)
        std::snprintf(buf, sizeof(buf), "%.1f Mo", mib);
    else
        std::snprintf(buf, sizeof(buf), "%.1f Ko", (double)bytes / 1024.0);
    std::string s = buf;
    const size_t dot = s.find('.');
    if (dot != std::string::npos) {
        s[dot] = ',';
        const size_t unit = s.find(' ');
        std::string num = s.substr(0, unit);
        if (num.size() >= 2 && num.compare(num.size() - 2, 2, ",0") == 0)
            num.erase(num.size() - 2);
        s = num + s.substr(unit);
    }
    return s;
}

// C# LoadMods/LoadFiles : Directory.GetFiles(...).OrderBy(f => f) (ordinal)
void sort_mod_paths(std::vector<std::string>& v) {
    std::sort(v.begin(), v.end());
}

// C# FindLogFile : game-log.txt puis logs/latest.log, sinon null
std::optional<std::string> resolve_log_file(const std::string& instDir) {
    std::error_code ec;
    const fs::path gameLog = fs::path(instDir) / "game-log.txt";
    if (fs::is_regular_file(gameLog, ec)) return gameLog.string();
    const fs::path latest = fs::path(instDir) / "logs" / "latest.log";
    if (fs::is_regular_file(latest, ec)) return latest.string();
    return std::nullopt;
}

} // namespace tl::ui::detail

#ifndef TL_DETAIL_LOGIC_ONLY
namespace tl::ui {

namespace {

// Etat de la modale (file-local : ui_internal.hpp non modifiable par ce portage)
std::string s_detailId;
int s_detailTab = 0; // 0 Infos, 1 Mods, 2 Mondes, 3 Journaux
bool s_detailRequest = false;
bool s_detailWasOpen = false;
std::string s_pendingEdit;
char s_logSearch[128] = "";
std::vector<std::string> s_logCache;
bool s_logDirty = true;

std::vector<std::string> list_mod_files(const fs::path& modsDir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return out;
    fs::directory_iterator it(modsDir, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        const std::string n = it->path().filename().string();
        std::string low = n;
        for (auto& c : low) c = (char)tolower((unsigned char)c);
        // C# "*.jar*" : .jar et .jar.disabled
        if (low.size() >= 4 && low.compare(low.size() - 4, 4, ".jar") == 0)
            out.push_back(it->path().string());
        // CORRECTIF : « .jar.disabled » fait 13 caracteres, pas 12. La
        // comparaison ne pouvait donc JAMAIS reussir : un mod desactive
        // disparaissait de la liste, et rien ne permettait plus de le
        // reactiver depuis l'interface.
        else if (low.size() > 13 &&
                 low.compare(low.size() - 13, 13, ".jar.disabled") == 0)
            out.push_back(it->path().string());
    }
    detail::sort_mod_paths(out);
    return out;
}

long long dir_size_bytes(const fs::path& d) {
    long long total = 0;
    std::error_code ec;
    fs::recursive_directory_iterator it(d, ec), end;
    for (; it != end; it.increment(ec)) {
        if (ec) break;
        if (it->is_regular_file(ec)) total += (long long)it->file_size(ec);
    }
    return total;
}

void reload_log_cache(const fs::path& file) {
    s_logCache.clear();
    std::ifstream in(file, std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        s_logCache.push_back(std::move(line));
    }
    // C# LoadLogs : 500 dernieres lignes
    if (s_logCache.size() > 500)
        s_logCache.erase(s_logCache.begin(), s_logCache.end() - 500);
}

void open_subfolder(const std::string& id, const char* sub) {
    std::error_code ec;
    fs::path d = DataStore::instancesRoot() / id;
    if (sub[0]) d /= sub;
    fs::create_directories(d, ec);
    open_in_explorer(d);
}

} // namespace

// C# InstancesPage.OnShowDetails(inst, tab) : id cible + onglet initial,
// ouverture effective dans instance_detail_modal().
void open_instance_detail(const std::string& id, int tab) {
    s_detailId = id;
    s_detailTab = (tab >= 0 && tab <= 3) ? tab : 0;
    s_logSearch[0] = '\0';
    s_logDirty = true;
    s_detailRequest = true;
}

void instance_detail_modal() {
    // Edition reportee d'une frame (evite d'empiler deux modales) — voir
    // open_edit_modal (C# OnEdit depuis InstanceDetailWindow.OnEdit).
    if (!s_pendingEdit.empty() && s_detailId.empty()) {
        open_edit_modal(s_pendingEdit);
        s_pendingEdit.clear();
    }
    if (s_detailId.empty() && !s_detailRequest) return;
    if (s_detailRequest) {
        ImGui::OpenPopup("###instdetail");
        s_detailRequest = false;
    }
    const ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f,
                        ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Appearing);
    bool open = true;
    const nlohmann::json* e = s_detailId.empty() ? nullptr : find_instance(s_detailId);
    const std::string nm = e ? e->value("Name", "?") : std::string("?");
    // Identifiant ImGui fixe apres ### (cf. instance_modals) : un changement
    // de langue ne ferme pas la modale.
    const std::string title = nm + "###instdetail";
    const bool began =
        ImGui::BeginPopupModal(title.c_str(), &open,
                               ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoResize);
    if (began) {
        s_detailWasOpen = true;
        const std::string id = s_detailId;

        // ---- banniere (C# RefreshData) ----
        if (fBig) ImGui::PushFont(fBig);
        ImGui::TextUnformatted(nm.c_str());
        if (fBig) ImGui::PopFont();
        if (e) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s · Minecraft %s · %d %s · %s", e->value("Loader", "").c_str(),
                e->value("McVersion", "").c_str(), e->value("Launches", 0),
                tr("lancé(s)"), format_playtime(e->value("PlaySeconds", 0LL)).c_str());
            const std::string notes = e->value("Notes", "");
            if (!notes.empty()) ImGui::TextWrapped("%s", notes.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::Spacing();

        // ---- onglets ----
        const char* tabs[] = {tr("Infos", "Info"), tr("Mods"),
                              tr("Mondes", "Worlds"), tr("Journaux", "Logs")};
        for (int t = 0; t < 4; ++t) {
            if (t) ImGui::SameLine();
            if (t == s_detailTab)
                accent_button(tabs[t], ImVec2(110, 30));
            else if (ImGui::Button(tabs[t], ImVec2(110, 30))) {
                s_detailTab = t;
                s_logDirty = true;
            }
        }
        ImGui::Separator();

        ImGui::BeginChild("##detailbody", ImVec2(-1, 330), false);
        const fs::path instDir =
            DataStore::instancesRoot() / id;
        if (s_detailTab == 0) {
            // ---- onglet Infos (C# LoadDescription) ----
            if (e) {
                const std::string desc = e->value("Description", "");
                ImGui::TextWrapped(
                    "%s", desc.empty()
                              ? tr("Pas de description pour cette instance.",
                                   "No description for this instance.")
                              : desc.c_str());
                ImGui::Spacing();
                const auto cnt = g.counts.count(id) ? g.counts.at(id)
                                                    : std::pair<int, int>{0, 0};
                ImGui::TextWrapped(tr("%d mod(s) · %d carte(s)",
                                      "%d mod(s) · %d map(s)"),
                                   cnt.first, cnt.second);
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped("%s : %s", tr("Loader"),
                                   e->value("Loader", "").c_str());
                ImGui::TextWrapped("Minecraft %s", e->value("McVersion", "").c_str());
                const int ram = e->value("MaxRamGb", 0);
                if (ram > 0)
                    ImGui::TextWrapped("RAM max : %d Go", ram);
                else
                    ImGui::TextWrapped("RAM max : %s", tr("globale", "global"));
                ImGui::TextWrapped(
                    tr("Temps de jeu : %s", "Play time: %s"),
                    format_playtime(e->value("PlaySeconds", 0LL)).c_str());
                ImGui::PopStyleColor();
            }
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s", tr("Shaders, resource packs, configs et screenshots : "
                         "non portés - utilisez le dossier de l'instance.",
                         "Shaders, resource packs, configs and screenshots: "
                         "not ported - use the instance folder."));
            ImGui::PopStyleColor();
        } else if (s_detailTab == 1) {
            // ---- Profils de lancement ----
            // Une meme instance sert a plusieurs usages : competitif avec
            // peu de mods, ou tranquille avec shaders. Un profil retient
            // l'etat des mods, la memoire et les arguments JVM.
            {
                auto* mut = find_instance(id);
                const fs::path modsDir = instDir / "mods";
                auto list = mut ? presets::load(*mut)
                                : std::vector<presets::Preset>{};
                const std::string cur = mut ? presets::active(*mut)
                                            : std::string();

                ImGui::TextUnformatted(tr("Profils de lancement",
                                          "Launch profiles"));
                static char s_newName[48] = "";
                static int s_newRam = 0;

                for (auto& p : list) {
                    ImGui::PushID(p.name.c_str());
                    const bool isCur = p.name == cur;
                    ImGui::PushStyleColor(ImGuiCol_Text, isCur ? kAccent : kText);
                    ImGui::TextUnformatted(p.name.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(220);
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    std::string meta;
                    if (p.ramGb > 0) meta += std::to_string(p.ramGb) + " Go";
                    if (p.hasMods) {
                        if (!meta.empty()) meta += " · ";
                        meta += std::to_string(p.disabled.size()) +
                                tr(" mod(s) off", " mod(s) off");
                    }
                    ImGui::TextUnformatted(meta.empty() ? "-" : meta.c_str());
                    ImGui::PopStyleColor();
                    ImGui::SameLine(380);
                    if (ImGui::SmallButton(isCur ? tr("Réappliquer", "Re-apply")
                                                 : tr("Appliquer", "Apply"))) {
                        if (mut) {
                            if (p.hasMods) {
                                const auto res = presets::apply_mods(
                                    modsDir, p.disabled,
                                    presets::all_mods(modsDir));
                                std::string msg =
                                    std::to_string(res.enabled) +
                                    tr(" activé(s), ", " enabled, ") +
                                    std::to_string(res.disabled) +
                                    tr(" désactivé(s)", " disabled");
                                if (res.untouched > 0)
                                    msg += tr(" · ", " - ") +
                                           std::to_string(res.untouched) +
                                           tr(" ajouté(s) depuis, non touché(s)",
                                              " added since, left alone");
                                if (res.failed > 0)
                                    msg += tr(" · ", " - ") +
                                           std::to_string(res.failed) +
                                           tr(" en échec", " failed");
                                notify_toast(tr("Profil", "Profile"), msg);
                            }
                            presets::set_active(*mut, p.name);
                            DataStore::save();
                            refresh_counts(id);
                        }
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton(tr("Supprimer", "Delete"))) {
                        if (mut) {
                            std::vector<presets::Preset> kept;
                            for (const auto& q : list)
                                if (q.name != p.name) kept.push_back(q);
                            presets::store(*mut, kept);
                            if (cur == p.name) presets::set_active(*mut, "");
                            DataStore::save();
                        }
                        ImGui::PopID();
                        break;
                    }
                    ImGui::PopID();
                }

                // Capture : on enregistre l'etat EN PLACE. C'est le geste
                // naturel — on regle ses mods, puis on nomme le resultat.
                ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
                ImGui::SetNextItemWidth(180.0f);
                ImGui::InputTextWithHint("##pname",
                                         tr("Nom du profil", "Profile name"),
                                         s_newName, sizeof(s_newName));
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::SetNextItemWidth(130.0f);
                ImGui::SliderInt("##pram", &s_newRam, 0, 32,
                                 s_newRam == 0 ? tr("RAM : instance",
                                                    "RAM: instance")
                                               : "%d Go");
                ImGui::SameLine();
                ImGui::BeginDisabled(!mut || s_newName[0] == '\0');
                if (ImGui::SmallButton(tr("Enregistrer l'état actuel",
                                          "Save current state"))) {
                    presets::Preset p;
                    p.name = s_newName;
                    p.ramGb = s_newRam;
                    p.hasMods = true;
                    p.disabled = presets::current_disabled(modsDir);
                    std::vector<presets::Preset> kept;
                    for (const auto& q : list)
                        if (q.name != p.name) kept.push_back(q);
                    kept.push_back(p);
                    presets::store(*mut, kept);
                    presets::set_active(*mut, p.name);
                    DataStore::save();
                    s_newName[0] = '\0';
                    notify_toast(tr("Profil", "Profile"),
                                 tr("Profil enregistré.", "Profile saved."));
                }
                ImGui::EndDisabled();
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped(
                    "%s",
                    tr("Le profil retient quels mods sont actifs. Un mod "
                       "installé après l'enregistrement n'est jamais "
                       "désactivé automatiquement.",
                       "A profile remembers which mods are enabled. A mod "
                       "installed after saving is never disabled "
                       "automatically."));
                ImGui::PopStyleColor();
                ImGui::Spacing();
                ImGui::Separator();
                ImGui::Spacing();
            }

            // ---- Compatibilite (phase 5) ----
            // Avant la liste, pas apres : ce qui empeche de jouer se lit
            // en premier, sans avoir a derouler quarante lignes.
            if (e) modcheck_panel(*e);
            if (e) modupdate_panel(*e);

            // ---- onglet Mods (C# LoadMods) ----
            const std::vector<std::string> mods =
                list_mod_files(instDir / "mods");
            if (mods.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped("%s", tr("Aucun mod installé.", "No mods installed."));
                ImGui::PopStyleColor();
            }
            for (const auto& f : mods) {
                ImGui::PushID(f.c_str());
                const fs::path p(f);
                const std::string fn = p.filename().string();
                const bool disabled = detail::mod_is_disabled(fn);
                std::error_code ec;
                const long long sz = fs::is_regular_file(p, ec)
                                         ? (long long)fs::file_size(p, ec)
                                         : 0;
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      disabled ? kDim : kText);
                ImGui::TextUnformatted(detail::mod_base_name(fn).c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(380);
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(detail::format_size_fr(sz).c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(480);
                if (ImGui::SmallButton(disabled ? tr("Activer", "Enable")
                                                : tr("Désactiver", "Disable"))) {
                    // C# : bascule par renommage .disabled
                    std::error_code ec2;
                    if (disabled)
                        fs::rename(p, p.parent_path() / detail::mod_base_name(fn),
                                   ec2);
                    else
                        fs::rename(p, fs::path(p.string() + ".disabled"), ec2);
                    refresh_counts(id);
                    // La liste des mods actifs vient de changer : le
                    // rapport de compatibilite affiche juste au-dessus ne
                    // vaut plus rien.
                    modcheck_invalidate();
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) {
                    std::error_code ec2;
                    fs::remove(p, ec2);
                    refresh_counts(id);
                    modcheck_invalidate();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", tr("Supprimer"));
                ImGui::PopID();
            }
            ImGui::Spacing();
            if (ImGui::Button(tr("Ouvrir le dossier mods", "Open mods folder")))
                open_subfolder(id, "mods");
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s", tr("Ajout : copiez le .jar dans le dossier "
                         "(sélecteur non porté). WorldEdit auto : non porté.",
                         "Adding: copy the .jar into the folder "
                         "(picker not ported). WorldEdit auto: not ported."));
            ImGui::PopStyleColor();
        } else if (s_detailTab == 2) {
            // ---- onglet Mondes (C# LoadWorlds sans WorldTools.level.dat) ----
            std::vector<std::string> worlds;
            {
                std::error_code ec;
                const fs::path saves = instDir / "saves";
                if (fs::is_directory(saves, ec)) {
                    fs::directory_iterator it(saves, ec), end;
                    for (; it != end; it.increment(ec)) {
                        if (ec) break;
                        if (!it->is_directory(ec)) continue;
                        const std::string n =
                            it->path().filename().string();
                        if (n.rfind("_backup_", 0) == 0) continue; // C#
                        worlds.push_back(it->path().string());
                    }
                    std::sort(worlds.begin(), worlds.end());
                }
            }
            if (worlds.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped(
                    "%s", tr("Aucun monde sauvegardé.", "No saved worlds."));
                ImGui::PopStyleColor();
            }
            for (const auto& w : worlds) {
                ImGui::PushID(w.c_str());
                const std::string n = fs::path(w).filename().string();
                ImGui::TextUnformatted(n.c_str());
                ImGui::SameLine(380);
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(
                    detail::format_size_fr(dir_size_bytes(w)).c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(480);
                if (ImGui::SmallButton(tr("Ouvrir", "Open")))
                    open_in_explorer(w);
                ImGui::PopID();
            }
            ImGui::Spacing();
            if (ImGui::Button(tr("Ouvrir le dossier saves", "Open saves folder")))
                open_subfolder(id, "saves");

            // ---- Sauvegardes ----
            // Le module backup existait mais n'etait appele NULLE PART dans
            // l'interface : aucune sauvegarde n'etait joignable.
            ImGui::Spacing();
            ImGui::Separator();
            ImGui::Spacing();
            if (fBig) ImGui::PushFont(fBig);
            ImGui::TextUnformatted(tr("Sauvegardes", "Backups"));
            if (fBig) ImGui::PopFont();

            static std::string s_confirmRestore; // archive en attente d'accord
            const auto backups = backup::list(id);

            ImGui::BeginDisabled(worlds.empty());
            if (accent_button(tr("Sauvegarder maintenant", "Back up now"),
                              ImVec2(230, 32))) {
                const std::string z = backup::create(id);
                if (z.empty()) {
                    notify_toast(tr("Sauvegardes", "Backups"),
                                 tr("Rien à sauvegarder.", "Nothing to back up."));
                } else {
                    const auto& st = DataStore::settings;
                    backup::rotate(id, st.backupKeep > 0 ? st.backupKeep
                                                         : backup::kMaxBackups,
                                   static_cast<long long>(st.backupSpaceMb) *
                                       1024 * 1024);
                    notify_toast(tr("Sauvegardes", "Backups"),
                                 tr("Sauvegarde créée.", "Backup created."));
                }
            }
            ImGui::EndDisabled();
            if (worlds.empty()) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(
                    tr("(aucun monde à sauvegarder)", "(no world to back up)"));
                ImGui::PopStyleColor();
            }

            if (backups.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped(
                    "%s",
                    tr("Aucune sauvegarde. La sauvegarde automatique se règle "
                       "dans Paramètres > Avancé.",
                       "No backup yet. Automatic backups are configured in "
                       "Settings > Advanced."));
                ImGui::PopStyleColor();
            }
            for (const auto& b : backups) {
                ImGui::PushID(b.file.string().c_str());
                // Apercu : date et taille. Sans eux, choisir quelle archive
                // restaurer revient a deviner.
                char when[64] = "?";
                const std::time_t t = static_cast<std::time_t>(b.mtime);
                if (std::tm* lt = std::localtime(&t))
                    std::strftime(when, sizeof(when), "%d/%m/%Y %H:%M", lt);
                ImGui::TextUnformatted(when);
                ImGui::SameLine(200);
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(detail::format_size_fr(b.bytes).c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine(320);
                if (ImGui::SmallButton(tr("Restaurer", "Restore")))
                    s_confirmRestore = b.file.string();
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Ouvrir", "Open")))
                    open_in_explorer(b.file.parent_path());
                ImGui::SameLine();
                if (ImGui::SmallButton(tr("Supprimer", "Delete")))
                    backup::remove(b.file);
                // Mondes contenus dans l'archive. Lire le sommaire d'un zip
                // coute une ouverture de fichier : on le retient, sinon ce
                // serait dix ouvertures par frame pour dix archives. La
                // date de modification sert de cle de fraicheur.
                {
                    static std::map<std::string, std::pair<long long,
                                                           std::vector<std::string>>>
                        s_contents;
                    const std::string k = b.file.string();
                    auto it = s_contents.find(k);
                    if (it == s_contents.end() || it->second.first != b.mtime)
                        it = s_contents
                                 .insert_or_assign(
                                     k, std::make_pair(b.mtime,
                                                       zip_top_level_dirs(b.file)))
                                 .first;
                    const auto& worldNames = it->second.second;
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    if (worldNames.empty()) {
                        ImGui::TextUnformatted(
                            tr("  (contenu illisible)", "  (unreadable)"));
                    } else {
                        std::string line = "  ";
                        for (size_t wi = 0; wi < worldNames.size(); ++wi) {
                            if (wi) line += ", ";
                            // Au-dela de quatre noms la ligne deborde : on
                            // compte le reste plutot que de la tronquer au
                            // milieu d'un nom.
                            if (wi == 4) {
                                line += tr("et ") +
                                        std::to_string(worldNames.size() - 4) +
                                        tr(" autre(s)", " more");
                                break;
                            }
                            line += worldNames[wi];
                        }
                        ImGui::TextWrapped("%s", line.c_str());
                    }
                    ImGui::PopStyleColor();
                }
                ImGui::PopID();
            }

            // Restauration : confirmation obligatoire. Elle REMPLACE le
            // dossier des mondes, donc tout monde cree depuis l'archive
            // disparait — ce n'est pas une fusion.
            if (!s_confirmRestore.empty()) {
                ImGui::OpenPopup("###bkrestore");
                const std::string title =
                    std::string(tr("Restaurer cette sauvegarde ?",
                                   "Restore this backup?")) +
                    "###bkrestore";
                const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f,
                               ImGui::GetIO().DisplaySize.y * 0.5f);
                ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::SetNextWindowSize(ImVec2(500, 210), ImGuiCond_Appearing);
                bool open = true;
                if (ImGui::BeginPopupModal(title.c_str(), &open,
                                           ImGuiWindowFlags_NoResize |
                                               ImGuiWindowFlags_NoCollapse)) {
                    ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
                    ImGui::TextUnformatted(
                        fs::path(s_confirmRestore).filename().string().c_str());
                    ImGui::Spacing();
                    ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
                    ImGui::TextUnformatted(
                        tr("Le dossier des mondes sera REMPLACÉ par le contenu "
                           "de l'archive. Les mondes créés depuis cette "
                           "sauvegarde seront perdus.",
                           "The worlds folder will be REPLACED by the archive's "
                           "contents. Worlds created since this backup will be "
                           "lost."));
                    ImGui::PopStyleColor();
                    ImGui::PopTextWrapPos();
                    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52.0f);
                    if (ImGui::Button(tr("Annuler"), ImVec2(150, 34))) {
                        s_confirmRestore.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (danger_button(tr("Restaurer", "Restore"),
                                      ImVec2(150, 34))) {
                        const bool ok = backup::restore(id, s_confirmRestore);
                        notify_toast(
                            tr("Sauvegardes", "Backups"),
                            ok ? tr("Mondes restaurés.", "Worlds restored.")
                               : tr("Restauration impossible : archive illisible.",
                                    "Restore failed: unreadable archive."));
                        s_confirmRestore.clear();
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
                if (!open) s_confirmRestore.clear();
            }

            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "%s", tr("Lecture level.dat et import CurseForge : non portés.",
                         "level.dat reading and CurseForge import: not ported."));
            ImGui::PopStyleColor();
        } else {
            // ---- onglet Journaux (C# LoadLogs/SearchLog) ----
            ImGui::SetNextItemWidth(300);
            ImGui::InputTextWithHint("##logsearch",
                                     tr("Rechercher dans le log...",
                                        "Search log..."),
                                     s_logSearch, sizeof(s_logSearch));
            // Filtre applique a l'affichage ci-dessous.
            ImGui::SameLine();
            if (ImGui::Button(tr("Rafraîchir", "Refresh"))) s_logDirty = true;
            const auto logFile = detail::resolve_log_file(instDir.string());
            if (!logFile) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextWrapped(
                    "%s", tr("Aucun journal. Lance Minecraft pour générer des logs.",
                             "No log. Launch Minecraft to generate logs."));
                ImGui::PopStyleColor();
            } else {
                if (s_logDirty) {
                    reload_log_cache(*logFile);
                    s_logDirty = false;
                }
                std::string needle = s_logSearch;
                for (auto& c : needle) c = (char)tolower((unsigned char)c);
                ImGui::BeginChild("##logview", ImVec2(-1, 230), true);
                for (const auto& line : s_logCache) {
                    if (!needle.empty()) {
                        std::string low = line;
                        for (auto& c : low) c = (char)tolower((unsigned char)c);
                        if (low.find(needle) == std::string::npos) continue;
                    }
                    // C# MakeLogRun : couleur par niveau
                    const bool isErr =
                        line.find("ERROR") != std::string::npos;
                    const bool isWarn =
                        !isErr && line.find("WARN") != std::string::npos;
                    if (isErr) ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
                    else if (isWarn)
                        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
                    ImGui::TextUnformatted(line.c_str());
                    if (isErr || isWarn) ImGui::PopStyleColor();
                }
                ImGui::EndChild();
            }
        }
        ImGui::EndChild();
        ImGui::Separator();

        // ---- actions (C# banniere + BuildActionButtons) ----
        const bool busy =
            g.phase == Phase::Preparing || g.phase == Phase::GameRunning;
        ImGui::BeginDisabled(busy);
        if (accent_button(tr("Jouer", "Play"), ImVec2(130, 36))) {
            g.selInstId = id;
            g.autoActive = false;
            g.page = 2;
            ImGui::CloseCurrentPopup();
            s_detailWasOpen = false;
            s_detailId.clear();
            start_worker();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button(tr("Modifier", "Edit"), ImVec2(130, 36))) {
            s_pendingEdit = id; // C# OnEdit (InstanceEditDialog non porte ici)
            ImGui::CloseCurrentPopup();
            s_detailWasOpen = false;
            s_detailId.clear();
        }
        ImGui::SameLine();
        if (ImGui::Button(tr("Dossier", "Folder"), ImVec2(130, 36)))
            open_subfolder(id, "");
        ImGui::SameLine();
        if (ImGui::Button(tr("Fermer", "Close"), ImVec2(130, 36))) {
            ImGui::CloseCurrentPopup();
            s_detailWasOpen = false;
            s_detailId.clear();
        }
        ImGui::EndPopup();
    } else if (s_detailWasOpen) {
        s_detailWasOpen = false;
        s_detailId.clear();
    }
}

} // namespace tl::ui
#endif

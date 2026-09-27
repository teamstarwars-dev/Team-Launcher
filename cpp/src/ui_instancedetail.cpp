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
        else if (low.size() > 12 &&
                 low.compare(low.size() - 12, 12, ".jar.disabled") == 0)
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
                         "non portés — utilisez le dossier de l'instance.",
                         "Shaders, resource packs, configs and screenshots: "
                         "not ported — use the instance folder."));
            ImGui::PopStyleColor();
        } else if (s_detailTab == 1) {
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
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("X")) {
                    std::error_code ec2;
                    fs::remove(p, ec2);
                    refresh_counts(id);
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

#include "ui_internal.hpp"

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Accueil (fidele HomePage C#) : greet + horloge, stats, hero, recents
// ---------------------------------------------------------------------------

void home_page() {
    auto& a = inst_array();
    auto& st = DataStore::settings;
    const bool busy = g.phase == Phase::Preparing || g.phase == Phase::GameRunning;

    // ---- Header : greet + horloge HH:mm ----
    const float availW = ImGui::GetContentRegionAvail().x;
    const ImVec2 lineStart = ImGui::GetCursorScreenPos();
    char clock[8];
    {
        std::time_t t = std::time(nullptr);
        std::tm tmv{};
#ifdef _WIN32
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        std::strftime(clock, sizeof(clock), "%H:%M", &tmv);
        ImGui::BeginGroup();
        const char* greet = tmv.tm_hour < 18 ? tr("Bonjour") : tr("Bonsoir");
        if (fBig) ImGui::PushFont(fBig);
        ImGui::Text("%s %s", greet,
                    st.playerName.empty() ? tr("Joueur") : st.playerName.c_str());
        if (fBig) ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("A quoi voulez-vous jouer aujourd'hui ?"));
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }
    {
        const ImVec2 cs = fBig ? fBig->CalcTextSizeA(fBig->FontSize, 10000.0f,
                                                     0.0f, clock)
                               : ImGui::CalcTextSize(clock);
        const ImVec2 rp(lineStart.x + availW - cs.x, lineStart.y);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (fBig)
            dl->AddText(fBig, fBig->FontSize, rp,
                        ImGui::ColorConvertFloat4ToU32(kDim), clock);
        else
            dl->AddText(rp, ImGui::ColorConvertFloat4ToU32(kDim), clock);
    }
    ImGui::Spacing();

    // ---- Stats (C# BuildStat) ----
    long long totalSec = 0;
    int totalLaunches = 0;
    int instCount = 0;
    if (a.is_array())
        for (const auto& e : a)
            if (e.is_object()) {
                ++instCount;
                totalSec += e.value("PlaySeconds", 0LL);
                totalLaunches += e.value("Launches", 0);
            }
    stat_card(std::to_string(instCount), tr("Instances"));
    ImGui::SameLine();
    stat_card(format_playtime(totalSec), tr("Temps de jeu"));
    ImGui::SameLine();
    stat_card(std::to_string(totalLaunches), tr("Lancements"));
    ImGui::Spacing();

    // ---- Etat vide ----
    if (!a.is_array() || a.empty()) {
        ImGui::BeginChild("##emptyhome", ImVec2(0, 230), ImGuiChildFlags_Borders);
        ImGui::Spacing();
        if (fBig) ImGui::PushFont(fBig);
        ImGui::TextUnformatted(tr("Aucune instance pour le moment"));
        if (fBig) ImGui::PopFont();
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("Créez une instance pour choisir une version de Minecraft et un "
                     "chargeur de mods.\nLes fichiers communs sont partagés entre "
                     "toutes vos instances, ce qui évite de télécharger plusieurs "
                     "fois les mêmes gigaoctets."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (accent_button(tr("Créer une instance"), ImVec2(210, 40))) {
            g.page = 1;
            g.modal = 1;
            g.modalRequest = true;
        }
        ImGui::EndChild();
        return;
    }

    // ---- Hero : instance la plus recente jouee (?? instances[0] en C#) ----
    nlohmann::json* hero = nullptr;
    for (auto& e : a) {
        if (!e.is_object() || !inst_played(e)) continue;
        if (!hero || e.value("LastPlayed", "") > hero->value("LastPlayed", ""))
            hero = &e;
    }
    if (!hero)
        for (auto& e : a)
            if (e.is_object()) {
                hero = &e;
                break;
            }
    if (!hero) return;
    const std::string heroId = hero->value("Id", "");

    ImGui::BeginChild("##hero", ImVec2(0, 116), ImGuiChildFlags_Borders);
    {
        const ImVec2 hp = ImGui::GetCursorScreenPos();
        const float hw = ImGui::GetContentRegionAvail().x;
        ImGui::BeginGroup();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(inst_last_label(*hero).c_str());
        ImGui::PopStyleColor();
        if (fBig) ImGui::PushFont(fBig);
        ImGui::TextUnformatted(hero->value("Name", "?").c_str());
        if (fBig) ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("%s - Minecraft %s", hero->value("Loader", "Vanilla").c_str(),
                    hero->value("McVersion", "?").c_str());
        ImGui::PopStyleColor();
        ImGui::EndGroup();

        const ImVec2 rBtn(hp.x + hw - 158.0f, hp.y + 34.0f);
        ImGui::SetCursorScreenPos(ImVec2(rBtn.x - 100.0f, rBtn.y));
        if (ImGui::Button(tr("Détails"), ImVec2(90, 42))) {
            g.selInstId = heroId;
            g.autoActive = false;
            g.page = 1;
        }
        ImGui::SetCursorScreenPos(rBtn);
        ImGui::BeginDisabled(busy);
        if (accent_button(tr("REPRENDRE"), ImVec2(150, 42))) {
            g.selInstId = heroId;
            g.autoActive = false;
            g.page = 2;
            start_worker();
        }
        ImGui::EndDisabled();
    }
    ImGui::EndChild();

    // ---- Recents (C# : hors hero, LastPlayed desc, puis Lancements, take 6) ----
    std::vector<const nlohmann::json*> recent;
    for (auto& e : a)
        if (e.is_object() && e.value("Id", "") != heroId) recent.push_back(&e);
    std::stable_sort(recent.begin(), recent.end(),
                     [](const nlohmann::json* x, const nlohmann::json* y) {
                         const bool px = inst_played(*x), py = inst_played(*y);
                         if (px != py) return px;
                         if (px && x->value("LastPlayed", "") !=
                                      y->value("LastPlayed", ""))
                             return x->value("LastPlayed", "") >
                                    y->value("LastPlayed", "");
                         return x->value("Launches", 0) > y->value("Launches", 0);
                     });
    if (recent.size() > 6) recent.resize(6);
    if (recent.empty()) return;

    ImGui::Spacing();
    const ImVec2 hdrPos = ImGui::GetCursorScreenPos();
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Vos instances"));
    if (fBig) ImGui::PopFont();
    {
        // « Tout gérer > » : texte aligné sur le bord droit + zone cliquable
        const char* seeAll = tr("Tout gérer >");
        const float sw = ImGui::CalcTextSize(seeAll).x;
        const float lh = ImGui::GetTextLineHeightWithSpacing();
        const ImVec2 tPos(hdrPos.x + availW - sw, hdrPos.y + 3.0f);
        const ImVec2 hMin(tPos.x - 8.0f, tPos.y - 4.0f);
        const ImVec2 hMax(tPos.x + sw + 8.0f, tPos.y + lh);
        const bool hovSee = ImGui::IsMouseHoveringRect(hMin, hMax);
        if (hovSee)
            ImGui::GetWindowDrawList()->AddRectFilled(
                hMin, hMax, ImGui::ColorConvertFloat4ToU32(hex(0x1a1a22)), 6.0f);
        ImGui::SetCursorScreenPos(tPos);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(seeAll);
        ImGui::PopStyleColor();
        if (hovSee) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) g.page = 1;
        }
        ImGui::SetCursorScreenPos(
            ImVec2(hdrPos.x, hdrPos.y + lh + 4.0f));
    }
    const ImVec2 gridStart = ImGui::GetCursorScreenPos();
    const float rcW = 240, rcH = 68, gap = 10;
    const int perRow =
        std::max(1, static_cast<int>((availW + gap) / (rcW + gap)));
    for (size_t i = 0; i < recent.size(); ++i) {
        const auto& e = *recent[i];
        const std::string id = e.value("Id", "");
        const int col = static_cast<int>(i) % perRow;
        const int row = static_cast<int>(i) / perRow;
        ImGui::SetCursorScreenPos(ImVec2(gridStart.x + col * (rcW + gap),
                                         gridStart.y + row * (rcH + gap)));
        ImGui::PushID(id.c_str());
        ImGui::BeginChild("##rc", ImVec2(rcW, rcH), ImGuiChildFlags_Borders);
        const bool hov = ImGui::IsWindowHovered();
        const ImVec2 cwp = ImGui::GetWindowPos();
        const ImVec2 cws = ImGui::GetWindowSize();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov)
            dl->AddRectFilled(cwp, ImVec2(cwp.x + cws.x, cwp.y + cws.y),
                              ImGui::ColorConvertFloat4ToU32(hex(0x1a1a22)),
                              6.0f);
        const std::string nm = e.value("Name", "?");
        dl->AddRectFilled(ImVec2(p.x, p.y + 4), ImVec2(p.x + 42, p.y + 46),
                          ImGui::ColorConvertFloat4ToU32(kAccent), 8.0f);
        {
            char initial[2] = {nm.empty() ? '?'
                                          : (char)toupper((unsigned char)nm[0]),
                               0};
            const ImVec2 ts = ImGui::CalcTextSize(initial);
            dl->AddText(ImVec2(p.x + 21 - ts.x / 2, p.y + 25 - ts.y / 2),
                        ImGui::ColorConvertFloat4ToU32(
                            ImVec4(0.055f, 0.055f, 0.075f, 1)),
                        initial);
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + 52, p.y + 10));
        ImGui::TextUnformatted(nm.c_str());
        if (fSmall) ImGui::PushFont(fSmall);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 52, p.y + 34));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("%s - %s", e.value("Loader", "").c_str(),
                    e.value("McVersion", "").c_str());
        ImGui::PopStyleColor();
        if (fSmall) ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x + rcW - 48, p.y + 16));
        ImGui::BeginDisabled(busy);
        if (icon_play_button(id.c_str(), 36)) {
            g.selInstId = id;
            g.autoActive = false;
            g.page = 2;
            start_worker();
        }
        ImGui::EndDisabled();
        if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
            !ImGui::IsAnyItemHovered()) {
            g.selInstId = id;
            g.autoActive = false;
            g.page = 1;
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    const int rows =
        (static_cast<int>(recent.size()) + perRow - 1) / perRow;
    ImGui::SetCursorScreenPos(
        ImVec2(gridStart.x, gridStart.y + rows * (rcH + gap)));
}

} // namespace tl::ui

#include "ui_internal.hpp"

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Jouer (console de lancement, selection d'instance)
// ---------------------------------------------------------------------------

void play_page() {
    auto& st = DataStore::settings;
    const bool busy = g.phase == Phase::Preparing || g.phase == Phase::GameRunning;

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    // Le mode compte suit desormais l'auth Microsoft (module 4a).
    ImGui::Text(st.accountMode == "microsoft"
                    ? tr("Joueur : %s  |  Compte Microsoft",
                         "Player: %s  |  Microsoft account")
                    : tr("Joueur : %s  |  Compte hors ligne",
                         "Player: %s  |  Offline account"),
                st.playerName.empty() ? tr("Joueur") : st.playerName.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    nlohmann::json* inst = selected_instance();
    if (!inst) {
        ImGui::TextUnformatted(tr("Aucune instance pour le moment."));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("Créez une instance pour choisir une version de Minecraft et "
                     "un chargeur de mods."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (accent_button(tr("Créer une instance"), ImVec2(220, 42))) {
            g.page = 1;
            g.modal = 1;
            g.modalRequest = true;
        }
        return;
    }

    // Selection d'instance
    ImGui::BeginDisabled(busy);
    {
        std::string preview = inst->value("Name", "?");
        if (g.autoActive) preview = "[test] " + preview;
        ImGui::SetNextItemWidth(340.0f);
        if (ImGui::BeginCombo("##instance", preview.c_str())) {
            if (g.autoActive && ImGui::Selectable("[test] instance de test")) {
                // deja selectionnee
            }
            for (auto& e : inst_array()) {
                if (!e.is_object()) continue;
                const std::string id = e.value("Id", "");
                const std::string lbl = e.value("Name", "?") + " · " +
                                        e.value("Loader", "") + " " +
                                        e.value("McVersion", "?") + "##" + id;
                if (ImGui::Selectable(lbl.c_str(),
                                      !g.autoActive && id == g.selInstId)) {
                    g.selInstId = id;
                    g.autoActive = false;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text(tr("%s · Minecraft %s · %d lancement(s) · %s",
                   "%s · Minecraft %s · %d launch(es) · %s"),
                inst->value("Loader", "Vanilla").c_str(),
                inst->value("McVersion", "?").c_str(),
                inst->value("Launches", 0),
                format_playtime(inst->value("PlaySeconds", 0LL)).c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // Bouton principal (accent) — etiquettes fideles v5
    const char* label = g.phase == Phase::GameRunning   ? "En jeu..."
                        : g.phase == Phase::Preparing    ? "Lancement..."
                                                         : "Jouer";
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::BeginDisabled(g.phase == Phase::Preparing || g.phase == Phase::GameRunning);
    if (ImGui::Button(label, ImVec2(-1.0f, 46.0f))) start_worker();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);

    if (g.phase == Phase::Preparing) {
        std::lock_guard<std::mutex> lk(g.m);
        const float frac =
            g.total > 0 ? static_cast<float>(g.done) / static_cast<float>(g.total)
                        : -1.0f;
        ImGui::ProgressBar(frac, ImVec2(-1.0f, 12.0f));
        ImGui::Text("%s", g.stage.empty() ? g.status.c_str() : g.stage.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
        if (ImGui::Button(tr("Annuler"), ImVec2(90.0f, 0.0f))) g.cancel = true;
    } else if (g.phase == Phase::Error) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", g.error.c_str());
        ImGui::PopStyleColor();
    } else {
        std::lock_guard<std::mutex> lk(g.m);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(g.status.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();

    // Journal
    ImGui::BeginChild("##journal", ImVec2(0, 0), ImGuiChildFlags_Borders);
    bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 8.0f;
    {
        std::lock_guard<std::mutex> lk(g.m);
        for (const auto& line : g.logLines) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", line.c_str());
            ImGui::PopStyleColor();
        }
    }
    if (atBottom && ImGui::GetScrollMaxY() > 0.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

} // namespace tl::ui

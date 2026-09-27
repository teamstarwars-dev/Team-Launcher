#include "ui_internal.hpp"

namespace tl::ui {

// ---------------------------------------------------------------------------
// Page Instances (fidele InstancesPage C#) : barre d'outils, grille, CRUD
// ---------------------------------------------------------------------------

// C# OnEditInstance : charge l'instance dans la modale d'edition
void open_edit_modal(const std::string& id) {
    if (auto* src = find_instance(id)) {
        std::snprintf(g.instName, sizeof(g.instName), "%s",
                      src->value("Name", "").c_str());
        std::snprintf(g.instDesc, sizeof(g.instDesc), "%s",
                      src->value("Description", "").c_str());
        std::snprintf(g.instVer, sizeof(g.instVer), "%s",
                      src->value("McVersion", "latest").c_str());
        const std::string ld = src->value("Loader", "Vanilla");
        g.instLoader = 0;
        for (int li = 0; li < 5; ++li)
            if (ld == kInstanceLoaders[li]) {
                g.instLoader = li;
                break;
            }
    }
    g.modalId = id;
    g.modal = 2;
    g.modalRequest = true;
}

void instances_page() {
    auto& a = inst_array();
    const bool busy = g.phase == Phase::Preparing || g.phase == Phase::GameRunning;

    // TL_AUTO_DETAIL=<onglet> : ouvre la modale détail de la 1re instance (test).
    {
        static bool autoDone = false;
        if (!autoDone) {
            autoDone = true;
            if (const char* t = std::getenv("TL_AUTO_DETAIL"))
                for (auto& e : a)
                    if (e.is_object()) {
                        const std::string aid = e.value("Id", "");
                        open_instance_detail(aid, std::atoi(t));
                        break;
                    }
        }
    }

    if (g.countsDirty) {
        g.countsDirty = false;
        for (auto& e : a)
            if (e.is_object()) refresh_counts(e.value("Id", ""));
    }

    // ---- Import de modpack en cours / resultat (module 4d) ----
    packs_frame();

    // ---- Barre d'outils ----
    if (accent_button(tr("+ Créer"), ImVec2(110, 34))) {
        g.instName[0] = '\0';
        g.instDesc[0] = '\0';
        std::snprintf(g.instVer, sizeof(g.instVer), "latest");
        g.instLoader = 0;
        g.modal = 1;
        g.modalRequest = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Importer"), ImVec2(110, 34)))
        ImGui::OpenPopup("import_popup");
    if (ImGui::BeginPopup("import_popup")) {
        if (ImGui::MenuItem(tr("Importer (.zip)"))) import_zip();
        if (ImGui::MenuItem(tr("Importer un dossier"))) import_folder();
        if (ImGui::MenuItem(tr("Modpack CurseForge / Modrinth...")))
            import_modpack_pick();
        if (ImGui::MenuItem(tr("Importer partagé (presse-papiers)")))
            import_shared_from_clipboard();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputTextWithHint("##search", tr("Rechercher une instance"), g.searchBuf,
                             sizeof(g.searchBuf));
    ImGui::SameLine();
    const char* sortLabels[] = {tr("Temps de jeu"), tr("Nom"), tr("Lancements"),
                                tr("Récemment jouée")};
    ImGui::SetNextItemWidth(170.0f);
    ImGui::Combo("##sort", &g.sortIdx, sortLabels, 4);
    ImGui::SameLine();
    if (ImGui::Button(g.smallGrid ? tr("Petit") : tr("Grand"), ImVec2(76, 34)))
        g.smallGrid = !g.smallGrid;
    ImGui::Spacing();

    // ---- Liste filtree + triee (C# FilterInstances / FilterList) ----
    std::vector<nlohmann::json*> list;
    std::string needle = g.searchBuf;
    for (auto& c : needle) c = (char)tolower((unsigned char)c);
    for (auto& e : a) {
        if (!e.is_object()) continue;
        if (!needle.empty()) {
            bool hit = false;
            for (const char* field : {"Name", "Loader", "McVersion"}) {
                std::string n = e.value(field, "");
                for (auto& c : n) c = (char)tolower((unsigned char)c);
                if (n.find(needle) != std::string::npos) {
                    hit = true;
                    break;
                }
            }
            if (!hit) continue;
        }
        list.push_back(&e);
    }
    std::stable_sort(
        list.begin(), list.end(), [&](const nlohmann::json* x,
                                      const nlohmann::json* y) {
            switch (g.sortIdx) {
            case 1: // OrdinalIgnoreCase (C#)
                return _stricmp(x->value("Name", "").c_str(),
                                y->value("Name", "").c_str()) < 0;
            case 2: return x->value("Launches", 0) > y->value("Launches", 0);
            case 3: {
                const bool px = inst_played(*x), py = inst_played(*y);
                if (px != py) return px;
                if (px)
                    return x->value("LastPlayed", "") > y->value("LastPlayed", "");
                return false;
            }
            default:
                return x->value("PlaySeconds", 0LL) > y->value("PlaySeconds", 0LL);
            }
        });

    if (list.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (a.is_array() && a.empty()) {
            ImGui::TextUnformatted(tr("Aucune instance"));
            ImGui::TextWrapped(tr("Créez-en une, ou déposez-en dans %s.", "Create one, or drop one into %s."),
                               DataStore::instancesRoot().string().c_str());
        } else {
            ImGui::TextWrapped("%s", tr("Rien ne correspond à votre recherche."));
            ImGui::TextWrapped("%s", tr("Essayez un autre terme, ou changez le tri."));
        }
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (accent_button(tr("+ Créer une instance"), ImVec2(210, 40))) {
            g.instName[0] = '\0';
            g.instDesc[0] = '\0';
            std::snprintf(g.instVer, sizeof(g.instVer), "latest");
            g.instLoader = 0;
            g.modal = 1;
            g.modalRequest = true;
        }
        return;
    }

    // ---- Grille de cartes (C# card 200x220 / 280x300, banner 90/120) ----
    const float cw = g.smallGrid ? 200.0f : 280.0f;
    const float ch = g.smallGrid ? 220.0f : 300.0f;
    const float bannerH = g.smallGrid ? 90.0f : 120.0f;
    const float gap = 14;
    const float pad = g.smallGrid ? 12.0f : 16.0f;
    const float availW = ImGui::GetContentRegionAvail().x;
    const int perRow =
        std::max(1, static_cast<int>((availW + gap) / (cw + gap)));
    const ImVec2 gridStart = ImGui::GetCursorScreenPos();
    const bool selectedBorder = true;

    for (size_t i = 0; i < list.size(); ++i) {
        const auto& e = *list[i];
        const std::string id = e.value("Id", "");
        const std::string nm = e.value("Name", "?");
        const int col = static_cast<int>(i) % perRow;
        const int row = static_cast<int>(i) / perRow;
        ImGui::SetCursorScreenPos(ImVec2(gridStart.x + col * (cw + gap),
                                         gridStart.y + row * (ch + gap)));
        ImGui::PushID(id.c_str());
        ImGui::BeginChild("##card", ImVec2(cw, ch), ImGuiChildFlags_Borders);
        const bool hov = ImGui::IsWindowHovered();
        const ImVec2 cwp = ImGui::GetWindowPos();
        const ImVec2 cws = ImGui::GetWindowSize();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (hov)
            dl->AddRectFilled(cwp, ImVec2(cwp.x + cws.x, cwp.y + cws.y),
                              ImGui::ColorConvertFloat4ToU32(hex(0x1a1a22)),
                              6.0f);
        if (selectedBorder && id == g.selInstId && !g.autoActive)
            dl->AddRect(cwp, ImVec2(cwp.x + cws.x, cwp.y + cws.y),
                        ImGui::ColorConvertFloat4ToU32(kAccent), 6.0f, 0, 1.5f);

        // banniere + lettre (C# MakeLetter)
        dl->AddRectFilled(cwp, ImVec2(cwp.x + cws.x, cwp.y + bannerH),
                          IM_COL32(0x10, 0x10, 0x18, 255));
        {
            char initial[2] = {nm.empty() ? '?'
                                          : (char)toupper((unsigned char)nm[0]),
                               0};
            const ImVec2 ts = fBig ? fBig->CalcTextSizeA(fBig->FontSize,
                                                         10000.0f, 0.0f, initial)
                                   : ImGui::CalcTextSize(initial);
            const ImVec2 lp(cwp.x + cws.x / 2 - ts.x / 2,
                            cwp.y + bannerH / 2 - ts.y / 2);
            if (fBig)
                dl->AddText(fBig, fBig->FontSize, lp,
                            ImGui::ColorConvertFloat4ToU32(kAccent), initial);
            else
                dl->AddText(lp, ImGui::ColorConvertFloat4ToU32(kAccent),
                            initial);
        }
        // overlay : Editer / Supprimer (en haut a droite de la banniere)
        {
            const float oy = cwp.y + 7;
            ImGui::SetCursorScreenPos(ImVec2(cwp.x + cws.x - 41, oy));
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (ImGui::Button("X", ImVec2(24, 22))) {
                g.modalId = id;
                g.modal = 3;
                g.modalRequest = true;
            }
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
                ImGui::SetTooltip("%s", tr("Supprimer"));
                ImGui::PopStyleColor();
            }
            ImGui::SetCursorScreenPos(ImVec2(cwp.x + cws.x - 109, oy));
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (ImGui::Button(tr("Editer"), ImVec2(64, 22))) open_edit_modal(id);
            ImGui::PopStyleColor();
        }

        // corps (C# StackPanel margin(pad,10) spacing 3)
        const float bodyY = cwp.y + bannerH + 10;
        const float bx = cwp.x + pad;
        ImGui::SetCursorScreenPos(ImVec2(bx, bodyY));
        ImGui::TextUnformatted(nm.c_str());
        if (fSmall) ImGui::PushFont(fSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::SetCursorScreenPos(ImVec2(bx, bodyY + 19));
        ImGui::TextWrapped("%s · Minecraft %s", e.value("Loader", "").c_str(),
                           e.value("McVersion", "").c_str());
        const auto cnt = g.counts.count(id) ? g.counts.at(id)
                                            : std::pair<int, int>{0, 0};
        ImGui::SetCursorScreenPos(ImVec2(bx, bodyY + 34));
        if (fSmall && fTiny) ImGui::PopFont(); // -> 10 px (C# FontSize 10)
        if (fTiny) ImGui::PushFont(fTiny);
        ImGui::TextWrapped(tr("%d mod(s) · %d carte(s) · %d lancé(s)",
                              "%d mod(s) · %d map(s) · %d launch(es)"),
                           cnt.first, cnt.second, e.value("Launches", 0));
        if (fTiny) ImGui::PopFont();
        if (fSmall) ImGui::PushFont(fSmall);
        // description (C# : grand uniquement, 2 lignes max)
        const std::string desc = e.value("Description", "");
        if (!g.smallGrid && desc.find_first_not_of(" \t\n\r") != std::string::npos) {
            ImGui::SetCursorScreenPos(ImVec2(bx, bodyY + 49));
            const ImVec2 clip0(bx, bodyY + 49);
            const ImVec2 clip1(bx + cws.x - pad * 2, bodyY + 49 + 30);
            ImGui::PushClipRect(clip0, clip1, true);
            ImGui::TextWrapped("%s", desc.c_str());
            ImGui::PopClipRect();
        }
        ImGui::PopStyleColor();
        if (fSmall) ImGui::PopFont();

        // Jouer pleine largeur (C# Height 34, margin pad / bas pad)
        ImGui::SetCursorScreenPos(
            ImVec2(cwp.x + pad, cwp.y + ch - 34 - pad));
        ImGui::BeginDisabled(busy);
        if (accent_button(tr("Jouer"), ImVec2(cw - pad * 2, 34))) {
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
        }
        // menu contextuel (C# ContextFlyout : clic droit sur toute la carte)
        const bool ctxOpen =
            (g.ctxAuto && i == 0) ||
            (ImGui::IsWindowHovered() &&
             ImGui::IsMouseClicked(ImGuiMouseButton_Right));
        if (ctxOpen) {
            ImGui::OpenPopup("inst_ctx");
            g.ctxAuto = false;
            ImGui::SetNextWindowPos(ImGui::GetMousePos(), ImGuiCond_Appearing);
        }
        if (ImGui::BeginPopup("inst_ctx")) {
            // C# AttachContextMenu : Détails / Mods / Mondes / Journaux /
            // Screenshots ouvrent InstanceDetailWindow (onglet initial).
            if (ImGui::MenuItem(tr("Détails", "Details")))
                open_instance_detail(id, 0);
            if (ImGui::MenuItem(tr("Mods"))) open_instance_detail(id, 1);
            if (ImGui::MenuItem(tr("Mondes", "Worlds")))
                open_instance_detail(id, 2);
            if (ImGui::MenuItem(tr("Journaux", "Logs")))
                open_instance_detail(id, 3);
            if (ImGui::MenuItem(tr("Screenshots"))) {
                // Onglet Screenshots non porté : repli dossier (C# OpenFolder).
                std::error_code ec;
                const std::filesystem::path d =
                    DataStore::instancesRoot() / id / "screenshots";
                std::filesystem::create_directories(d, ec);
                open_in_explorer(d);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Modifier"))) open_edit_modal(id);
            if (ImGui::MenuItem(tr("Ouvrir"))) {
                std::error_code ec;
                const std::filesystem::path d = DataStore::instancesRoot() / id;
                std::filesystem::create_directories(d, ec);
                open_in_explorer(d);
            }
            if (ImGui::MenuItem(tr("Exporter .zip"))) export_zip(e);
            if (ImGui::MenuItem(tr("Partager (copier le pack)"))) share_instance_start(e);
            ImGui::Separator();
            if (ImGui::MenuItem(tr("Dupliquer"))) duplicate_instance(e);
            if (ImGui::MenuItem(tr("Supprimer"))) {
                g.modalId = id;
                g.modal = 3;
                g.modalRequest = true;
            }
            ImGui::EndPopup();
        }
        ImGui::EndChild();
        ImGui::PopID();
    }
    const int rows = (static_cast<int>(list.size()) + perRow - 1) / perRow;
    ImGui::SetCursorScreenPos(
        ImVec2(gridStart.x, gridStart.y + rows * (ch + gap)));
}

// ---------------------------------------------------------------------------
// Modales instance (creer / editer / supprimer)
// ---------------------------------------------------------------------------

void instance_modals() {
    // Les titres sont traduits mais l'identifiant ImGui (apres ###) reste fixe :
    // changer de langue ne doit pas fermer la modale ouverte.
    if (g.modalRequest && g.modal) {
        ImGui::OpenPopup(g.modal == 3 ? "###instdelete" : "###instmodal");
        g.modalRequest = false;
    }
    const ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f,
                        ImGui::GetIO().DisplaySize.y * 0.5f);
    auto label = [](const char* t) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(t);
        ImGui::PopStyleColor();
    };

    if (g.modal == 1 || g.modal == 2) {
        const bool creating = g.modal == 1;
        const std::string title =
            std::string(creating ? tr("Créer une instance") : tr("Éditer l'instance")) +
            "###instmodal";
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(500, 440), ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::BeginPopupModal(title.c_str(), &open,
                                   ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoResize)) {
            label(tr("Nom"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##iname", g.instName, sizeof(g.instName));
            label(tr("Description"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextMultiline("##idesc", g.instDesc, sizeof(g.instDesc),
                                      ImVec2(-1, 70));
            label(tr("Loader"));
            ImGui::SetNextItemWidth(-1);
            ImGui::Combo("##iloader", &g.instLoader, kInstanceLoaders, 5);
            label(tr("Version"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##iver", g.instVer, sizeof(g.instVer));
            ImGui::Spacing();
            const std::string name = trimmed(g.instName);
            if (name.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(tr("Le nom est requis."));
                ImGui::PopStyleColor();
            }
            if (accent_button(creating ? tr("Créer") : tr("Enregistrer"),
                              ImVec2(130, 36)) &&
                !name.empty()) {
                std::string ver = trimmed(g.instVer);
                if (ver.empty()) ver = "latest";
                if (creating) {
                    nlohmann::json e = {{"Id", new_guid()},
                                        {"Name", name},
                                        {"Description", trimmed(g.instDesc)},
                                        {"ImagePath", ""},
                                        {"Loader", kInstanceLoaders[g.instLoader]},
                                        {"McVersion", ver},
                                        {"Launches", 0},
                                        {"PlaySeconds", 0},
                                        {"MaxRamGb", 0},
                                        {"JvmArgs", ""},
                                        {"Notes", ""},
                                        {"LastPlayed", "0001-01-01T00:00:00"}};
                    const std::string id = e.value("Id", "");
                    inst_array().push_back(std::move(e));
                    g.selInstId = id;
                    g.autoActive = false;
                    refresh_counts(id);
                    push_log("Instance « " + name + " » créée.");
                } else {
                    if (auto* it = find_instance(g.modalId)) {
                        (*it)["Name"] = name;
                        (*it)["Description"] = trimmed(g.instDesc);
                        (*it)["Loader"] = kInstanceLoaders[g.instLoader];
                        (*it)["McVersion"] = ver;
                        refresh_counts(g.modalId);
                        push_log("Instance « " + name + " » mise à jour.");
                    }
                }
                DataStore::save();
                g.modal = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Annuler"), ImVec2(130, 36))) {
                g.modal = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else if (g.modal == 1 || g.modal == 2) {
            g.modal = 0; // fermee (Echap / croix)
        }
    } else if (g.modal == 3) {
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(460, 190), ImGuiCond_Appearing);
        bool open = true;
        const std::string delTitle =
            std::string(tr("Supprimer l'instance")) + "###instdelete";
        if (ImGui::BeginPopupModal(delTitle.c_str(), &open,
                                   ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoResize)) {
            const auto* target = find_instance(g.modalId);
            const std::string nm =
                target ? target->value("Name", "?") : std::string("?");
            ImGui::Text(tr("Supprimer l'instance « %s » ?", "Delete instance \"%s\"?"), nm.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped(
                "Son dossier et ses mondes seront définitivement supprimés.");
            ImGui::PopStyleColor();
            ImGui::Spacing();
            if (danger_button(tr("Supprimer"), ImVec2(130, 36))) {
                auto& arr = inst_array();
                for (auto it = arr.begin(); it != arr.end(); ++it)
                    if (it->is_object() && it->value("Id", "") == g.modalId) {
                        std::error_code ec;
                        std::filesystem::remove_all(
                            DataStore::instancesRoot() / g.modalId, ec);
                        arr.erase(it);
                        break;
                    }
                if (g.selInstId == g.modalId) g.selInstId.clear();
                g.counts.erase(g.modalId);
                DataStore::save();
                push_log("Instance « " + nm + " » supprimée.");
                g.modal = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(tr("Annuler"), ImVec2(130, 36))) {
                g.modal = 0;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        } else if (g.modal == 3) {
            g.modal = 0;
        }
    }
}

} // namespace tl::ui

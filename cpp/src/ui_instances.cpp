#include "ui_internal.hpp"

#include "icons.hpp"

#include "util_str.hpp" // strCaseCmp (tri insensible à la casse)

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

    // ---- Filtres par tag ----
    // Les tags existants sont deduits des instances : pas de liste a gerer
    // a part, donc rien a nettoyer quand une instance disparait.
    {
        std::vector<std::string> tags;
        for (auto& e : a) {
            if (!e.is_object()) continue;
            const auto it = e.find("Tags");
            if (it == e.end() || !it->is_array()) continue;
            for (const auto& t : *it) {
                if (!t.is_string()) continue;
                const std::string v = t.get<std::string>();
                if (v.empty()) continue;
                if (std::find(tags.begin(), tags.end(), v) == tags.end())
                    tags.push_back(v);
            }
        }
        if (!tags.empty()) {
            std::sort(tags.begin(), tags.end(), [](const auto& x, const auto& y) {
                return tl::strCaseCmp(x.c_str(), y.c_str()) < 0;
            });
            ImGui::Spacing();
            const bool none = g.tagFilter.empty();
            ImGui::PushStyleColor(ImGuiCol_Button, none ? kAccent : kButton);
            if (ImGui::SmallButton(tr("Tous", "All"))) g.tagFilter.clear();
            ImGui::PopStyleColor();
            for (const auto& t : tags) {
                ImGui::SameLine();
                const bool on = g.tagFilter == t;
                ImGui::PushStyleColor(ImGuiCol_Button, on ? kAccent : kButton);
                if (ImGui::SmallButton(t.c_str()))
                    g.tagFilter = on ? std::string() : t; // re-clic = tout
                ImGui::PopStyleColor();
            }
        }
    }
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
            // La recherche porte aussi sur les tags : taper « pvp » doit
            // ramener les instances marquees ainsi, pas seulement celles
            // dont le nom contient « pvp ».
            if (!hit)
                if (const auto it = e.find("Tags");
                    it != e.end() && it->is_array())
                    for (const auto& t : *it) {
                        if (!t.is_string()) continue;
                        std::string n = t.get<std::string>();
                        for (auto& c : n) c = (char)tolower((unsigned char)c);
                        if (n.find(needle) != std::string::npos) {
                            hit = true;
                            break;
                        }
                    }
            if (!hit) continue;
        }
        if (!g.tagFilter.empty()) {
            bool tagged = false;
            if (const auto it = e.find("Tags"); it != e.end() && it->is_array())
                for (const auto& t : *it)
                    if (t.is_string() && t.get<std::string>() == g.tagFilter) {
                        tagged = true;
                        break;
                    }
            if (!tagged) continue;
        }
        list.push_back(&e);
    }
    std::stable_sort(
        list.begin(), list.end(), [&](const nlohmann::json* x,
                                      const nlohmann::json* y) {
            // Les favoris passent devant, quel que soit le tri choisi :
            // c'est tout l'interet de les epingler. A egalite, le tri
            // demande s'applique normalement.
            const bool fx = x->value("Favorite", false);
            const bool fy = y->value("Favorite", false);
            if (fx != fy) return fx;
            switch (g.sortIdx) {
            case 1: // OrdinalIgnoreCase (C#)
                return tl::strCaseCmp(x->value("Name", "").c_str(),
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
                              ImGui::ColorConvertFloat4ToU32(
                                  shade_by(kCard, 0.05f)),
                              6.0f);
        if (selectedBorder && id == g.selInstId && !g.autoActive)
            dl->AddRect(cwp, ImVec2(cwp.x + cws.x, cwp.y + cws.y),
                        ImGui::ColorConvertFloat4ToU32(kAccent), 6.0f, 0, 1.5f);

        // banniere + lettre (C# MakeLetter)
        // Banniere : legerement en retrait de la carte, dans les deux sens
        // selon le theme. En dur (0x101018) elle faisait une bande noire sur
        // une carte blanche en theme clair.
        dl->AddRectFilled(cwp, ImVec2(cwp.x + cws.x, cwp.y + bannerH),
                          ImGui::ColorConvertFloat4ToU32(
                              shade_by(kCard, 0.04f)));
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

            // Etoile des favoris, en haut a GAUCHE de la banniere. Sans
            // marque visible, on ne comprend pas pourquoi une instance
            // passe devant alors que le tri porte sur autre chose.
            // Cliquable : epingler ne doit pas obliger a ouvrir le menu
            // contextuel.
            const bool fav = e.value("Favorite", false);
            ImGui::SetCursorScreenPos(ImVec2(cwp.x + 7, oy));
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.18f));
            const ImVec2 sp = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##fav", ImVec2(24, 22))) {
                if (auto* m = find_instance(id)) {
                    (*m)["Favorite"] = !fav;
                    DataStore::save();
                }
            }
            ImGui::PopStyleColor(3);
            const bool hov = ImGui::IsItemHovered();
            // Etoile pleine si favori, simple contour sinon — et l'etoile
            // en creux n'apparait qu'au survol, pour ne pas encombrer
            // toutes les cartes.
            if (fav || hov) {
                icons::draw(ImGui::GetWindowDrawList(), icons::Id::Star,
                            ImVec2(sp.x + 5, sp.y + 4), 14.0f,
                            ImGui::ColorConvertFloat4ToU32(
                                fav ? ImVec4(1.0f, 0.78f, 0.24f, 1.0f) : kDim));
            }
            if (hov)
                ImGui::SetTooltip("%s", fav ? tr("Retirer des favoris",
                                                 "Remove from favorites")
                                            : tr("Épingler en favori",
                                                 "Pin as favorite"));
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
            {
                const bool fav = e.value("Favorite", false);
                if (ImGui::MenuItem(fav ? tr("Retirer des favoris",
                                             "Remove from favorites")
                                        : tr("Épingler en favori",
                                             "Pin as favorite"))) {
                    // `e` est une reference CONSTANTE dans cette boucle :
                    // on repasse par la recherche mutable pour ecrire.
                    if (auto* m = find_instance(id)) {
                        (*m)["Favorite"] = !fav;
                        DataStore::save();
                    }
                }
            }
            if (ImGui::MenuItem(tr("Tags..."))) {
                g.tagsId = id;
                // Pre-remplit le champ avec les tags existants.
                std::string joined;
                if (const auto it = e.find("Tags");
                    it != e.end() && it->is_array())
                    for (const auto& t : *it)
                        if (t.is_string()) {
                            if (!joined.empty()) joined += ", ";
                            joined += t.get<std::string>();
                        }
                std::snprintf(g.tagsBuf, sizeof(g.tagsBuf), "%s", joined.c_str());
                g.tagsRequest = true;
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

// Modale d'edition des tags d'une instance. Saisie libre separee par des
// virgules plutot qu'une liste a cocher : les categories sont propres a
// chaque utilisateur, on ne peut pas les deviner a l'avance.
void tags_modal() {
    if (g.tagsRequest) {
        ImGui::OpenPopup("###insttags");
        g.tagsRequest = false;
    }
    const ImVec2 center(ImGui::GetIO().DisplaySize.x * 0.5f,
                        ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 230), ImGuiCond_Appearing);
    const std::string title = std::string(tr("Tags")) + "###insttags";
    bool open = true;
    if (!ImGui::BeginPopupModal(title.c_str(), &open,
                                ImGuiWindowFlags_NoResize |
                                    ImGuiWindowFlags_NoCollapse))
        return;

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s", tr("Sépare les tags par des virgules. Ils servent à filtrer la "
                 "liste et sont pris en compte par la recherche.",
                 "Separate tags with commas. They filter the list and are "
                 "included in the search."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##tagsedit", "pvp, survie, avec les copains",
                             g.tagsBuf, sizeof(g.tagsBuf));
    ImGui::PopStyleColor();

    ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52.0f);
    if (ImGui::Button(tr("Annuler"), ImVec2(150, 34))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    if (accent_button(tr("Enregistrer", "Save"), ImVec2(150, 34))) {
        if (auto* e = find_instance(g.tagsId)) {
            nlohmann::json arr = nlohmann::json::array();
            std::string cur;
            auto flush = [&] {
                // Espaces de bordure retires : « pvp , survie » et
                // « pvp,survie » doivent donner les memes tags, sinon le
                // filtre se retrouve avec des doublons invisibles.
                const auto b = cur.find_first_not_of(" \t");
                const auto f = cur.find_last_not_of(" \t");
                if (b != std::string::npos) {
                    const std::string t = cur.substr(b, f - b + 1);
                    bool dup = false;
                    for (const auto& x : arr)
                        if (x.get<std::string>() == t) dup = true;
                    if (!dup) arr.push_back(t);
                }
                cur.clear();
            };
            for (const char* p = g.tagsBuf; *p; ++p) {
                if (*p == ',')
                    flush();
                else
                    cur.push_back(*p);
            }
            flush();
            if (arr.empty())
                e->erase("Tags"); // pas de tableau vide qui traine
            else
                (*e)["Tags"] = arr;
            DataStore::save();
        }
        // Un tag peut avoir disparu : ne pas laisser un filtre sur un tag
        // qui n'existe plus, sinon la liste parait vide sans raison.
        g.tagFilter.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void instance_modals() {
    tags_modal();
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

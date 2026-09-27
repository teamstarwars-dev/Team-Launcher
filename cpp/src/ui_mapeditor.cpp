#include "ui_internal.hpp"

#include "apptasks.hpp"
#include "region.hpp"
#include "world.hpp"
#include "worldedit.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <set>

// ---------------------------------------------------------------------------
// Editeur de cartes (portage d'EditorCanvas.cs / MapEditorPage.cs) : grille des
// chunks d'un monde, selection au glisser, suppression de la selection.
//
// Les operations WorldEdit (pos1/pos2, //set, //replace, //copy, //paste,
// //undo, //redo) vivent dans `worldedit.hpp` : cette page n'en est que la
// facade. Double-clic sur un chunk = pos1, Maj+double-clic = pos2, au centre
// du chunk et a la hauteur du plus haut bloc non-air (comme le C#).
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct RegionCells {
    int rx = 0, rz = 0;
    std::vector<std::uint8_t> present; // 1024 cases : chunk present ou non
    int count = 0;
};

struct MapState {
    std::string instId;
    std::filesystem::path worldPath;
    std::vector<world::Info> worlds;
    bool worldsDirty = true;

    std::vector<RegionCells> regions;
    int minRx = 0, minRz = 0, maxRx = 0, maxRz = 0;
    int totalChunks = 0;
    bool loaded = false;

    std::set<std::pair<int, int>> selected; // coordonnees monde (cx, cz)

    float zoom = 0.0f;   // 0 = ajuster a la vue
    ImVec2 pan{0, 0};
    bool dragSel = false;
    bool dragErase = false; // clic droit : deselectionner
    ImVec2 dragStart{0, 0};

    std::string status;
    bool confirmDelete = false;

    // --- WorldEdit ---
    std::optional<worldedit::Pos> pos1;
    std::optional<worldedit::Pos> pos2;
    char blockSet[64] = "minecraft:stone";
    char blockFrom[64] = "*";
    char blockTo[64] = "minecraft:air";
    worldedit::History hist;
    worldedit::Clipboard clip;

    std::mutex wm;          // protege les champs ci-dessous
    bool weRunning = false;
    std::string weStatus;
    std::string weResult;
    bool weOk = false;
    // Compteurs recopies a la fin de chaque operation : l UI ne touche jamais
    // `hist` ni `clip` pendant qu un worker les modifie.
    std::size_t undoN = 0, redoN = 0, histKo = 0, clipN = 0;
};
MapState M;

// Hauteur du plus haut bloc non-air d'une colonne, 64 par defaut.
int top_y(const std::filesystem::path& worldDir, int bx, int bz) {
    const int cx = static_cast<int>(std::floor(bx / 16.0));
    const int cz = static_cast<int>(std::floor(bz / 16.0));
    auto chunk = region::read_chunk(worldDir, cx, cz);
    if (!chunk) return 64;
    // CORRECTIF vs C# : celui-ci reconstruisait tous les blocs du chunk puis
    // sondait 384 hauteurs. `heightmap` calcule par section, sans materialiser.
    const auto hm = region::heightmap(*chunk);
    const int lx = ((bx % 16) + 16) % 16;
    const int lz = ((bz % 16) + 16) % 16;
    const std::int32_t h = hm[static_cast<std::size_t>(lz) * 16 + lx];
    return h == INT32_MIN ? 64 : h;
}

void load_world(const std::filesystem::path& worldDir) {
    M.regions.clear();
    M.selected.clear();
    M.totalChunks = 0;
    M.loaded = false;
    M.zoom = 0.0f;
    M.pan = ImVec2(0, 0);
    // L'historique et le presse-papier portent des coordonnees et des chunks
    // du monde precedent : les garder d'un monde a l'autre corromprait le
    // nouveau au premier //undo.
    if (M.worldPath != worldDir) {
        M.pos1.reset();
        M.pos2.reset();
        M.hist.clear();
        M.clip = worldedit::Clipboard{};
        std::lock_guard<std::mutex> lk(M.wm);
        M.undoN = M.redoN = M.histKo = M.clipN = 0;
        M.weResult.clear();
    }
    M.worldPath = worldDir;
    if (worldDir.empty()) return;

    // Seuls les en-tetes sont lus : instantane meme sur un monde de 700 Mo.
    for (const auto& f : region::list_regions(worldDir)) {
        RegionCells rc;
        rc.rx = f.rx;
        rc.rz = f.rz;
        rc.present.assign(1024, 0);
        for (const auto& c : region::list_chunks(f.path)) {
            rc.present[static_cast<std::size_t>(c.localZ) * 32 + c.localX] = 1;
            ++rc.count;
        }
        M.totalChunks += rc.count;
        M.regions.push_back(std::move(rc));
    }
    if (M.regions.empty()) return;

    M.minRx = M.maxRx = M.regions[0].rx;
    M.minRz = M.maxRz = M.regions[0].rz;
    for (const auto& r : M.regions) {
        M.minRx = (std::min)(M.minRx, r.rx);
        M.maxRx = (std::max)(M.maxRx, r.rx);
        M.minRz = (std::min)(M.minRz, r.rz);
        M.maxRz = (std::max)(M.maxRz, r.rz);
    }
    M.loaded = true;
}

void refresh_worlds() {
    M.worlds.clear();
    M.worldsDirty = false;
    if (M.instId.empty()) return;
    M.worlds = world::list_worlds(DataStore::instancesRoot() / M.instId);
}

// --- rendu de la grille ----------------------------------------------------

void draw_canvas() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, (std::max)(avail.y - 8.0f, 200.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32(kBg));
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(kBorder));

    ImGui::InvisibleButton("##mapcanvas", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();

    if (!M.loaded) {
        const char* msg = tr("Aucun monde chargé.", "No world loaded.");
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(kDim), msg);
        return;
    }

    const int chunksX = (M.maxRx - M.minRx + 1) * 32;
    const int chunksZ = (M.maxRz - M.minRz + 1) * 32;

    // Zoom d'ajustement au premier affichage, puis molette.
    const float fit = (std::min)((size.x - 20.0f) / chunksX,
                                 (size.y - 20.0f) / chunksZ);
    // Pas de plancher haut sur l ajustement : un monde tres etendu (une region
    // egaree a 19 500 regions du centre, cas reel rencontre) doit tenir dans la
    // vue, quitte a etre minuscule. L utilisateur zoome ensuite.
    if (M.zoom <= 0.0f) M.zoom = (std::max)(fit, 0.0005f);
    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            const float before = M.zoom;
            M.zoom = std::clamp(M.zoom * (wheel > 0 ? 1.2f : 1.0f / 1.2f), 0.02f,
                                40.0f);
            // Zoom centre sur le curseur : le point sous la souris ne bouge pas.
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const float k = M.zoom / before;
            M.pan.x = mp.x - p0.x - (mp.x - p0.x - M.pan.x) * k;
            M.pan.y = mp.y - p0.y - (mp.y - p0.y - M.pan.y) * k;
        }
        // Glisser au bouton du milieu : deplacement.
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            M.pan.x += d.x;
            M.pan.y += d.y;
        }
    }

    const float cell = M.zoom;
    const float ox = p0.x + M.pan.x + (size.x - chunksX * cell) * 0.5f;
    const float oy = p0.y + M.pan.y + (size.y - chunksZ * cell) * 0.5f;

    auto cell_pos = [&](int cx, int cz) {
        return ImVec2(ox + (cx - M.minRx * 32) * cell,
                      oy + (cz - M.minRz * 32) * cell);
    };
    auto chunk_at = [&](ImVec2 m) {
        const float fx = (m.x - ox) / cell;
        const float fy = (m.y - oy) / cell;
        return std::pair<int, int>(
            static_cast<int>(std::floor(fx)) + M.minRx * 32,
            static_cast<int>(std::floor(fy)) + M.minRz * 32);
    };

    dl->PushClipRect(p0, p1, true);

    const ImU32 colChunk = ImGui::ColorConvertFloat4ToU32(kAccent);
    const ImU32 colSel = IM_COL32(255, 150, 40, 255); // orange du C#
    const ImU32 colRegion = ImGui::ColorConvertFloat4ToU32(kBorder);

    // Sous 2 px par chunk, dessiner 1024 rectangles par region est inutile et
    // couteux (Greenfield : 139 regions). On remplit la region en degrade
    // selon sa densite de chunks.
    const bool coarse = cell < 2.0f;

    for (const auto& r : M.regions) {
        const ImVec2 rp = cell_pos(r.rx * 32, r.rz * 32);
        const ImVec2 rq(rp.x + 32 * cell, rp.y + 32 * cell);
        if (rq.x < p0.x || rp.x > p1.x || rq.y < p0.y || rp.y > p1.y)
            continue; // region hors ecran

        if (coarse) {
            // Sous le pixel, un rectangle ne dessine rien : on garantit une
            // taille minimale pour qu une region isolee reste reperable.
            const float vis = (std::max)(32 * cell, 2.0f);
            const ImVec2 rq2(rp.x + vis, rp.y + vis);
            const float density = r.count / 1024.0f;
            const int a = 40 + static_cast<int>(density * 180.0f);
            dl->AddRectFilled(rp, rq2,
                              IM_COL32(static_cast<int>(kAccent.x * 255),
                                       static_cast<int>(kAccent.y * 255),
                                       static_cast<int>(kAccent.z * 255), a));
            if (vis > 3.0f) dl->AddRect(rp, rq2, colRegion);
            continue;
        }

        dl->AddRect(rp, rq, colRegion);
        const float sz = (std::max)(cell - 0.5f, 1.0f);
        for (int lz = 0; lz < 32; ++lz) {
            for (int lx = 0; lx < 32; ++lx) {
                if (!r.present[static_cast<std::size_t>(lz) * 32 + lx]) continue;
                const int cx = r.rx * 32 + lx, cz = r.rz * 32 + lz;
                const ImVec2 q = cell_pos(cx, cz);
                if (q.x > p1.x || q.y > p1.y || q.x + sz < p0.x || q.y + sz < p0.y)
                    continue;
                const bool sel = M.selected.count({cx, cz}) != 0;
                dl->AddRectFilled(q, ImVec2(q.x + sz, q.y + sz),
                                  sel ? colSel : colChunk);
            }
        }
    }

    // --- pos1 / pos2 : double-clic (Maj = pos2), centre du chunk ---
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        const auto c = chunk_at(ImGui::GetIO().MousePos);
        const int bx = c.first * 16 + 8, bz = c.second * 16 + 8;
        const worldedit::Pos p{bx, top_y(M.worldPath, bx, bz), bz};
        if (ImGui::GetIO().KeyShift)
            M.pos2 = p;
        else
            M.pos1 = p;
        M.dragSel = false; // le clic simple avait amorce un glisser
    }

    // Reperes pos1 (rouge) et pos2 (vert), au chunk qui les contient.
    auto draw_marker = [&](const worldedit::Pos& p, ImU32 col, const char* tag) {
        const ImVec2 q = cell_pos(static_cast<int>(std::floor(p.x / 16.0)),
                                  static_cast<int>(std::floor(p.z / 16.0)));
        const float s = (std::max)(cell, 8.0f);
        dl->AddRect(ImVec2(q.x - 1, q.y - 1), ImVec2(q.x + s + 1, q.y + s + 1),
                    col, 0.0f, 0, 2.0f);
        dl->AddText(ImVec2(q.x + s + 3, q.y - 2), col, tag);
    };
    if (M.pos1) draw_marker(*M.pos1, IM_COL32(230, 70, 70, 255), "1");
    if (M.pos2) draw_marker(*M.pos2, IM_COL32(80, 220, 110, 255), "2");

    // --- selection au glisser ---
    if (hovered && !coarse) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            M.dragSel = true;
            M.dragErase = ImGui::IsMouseClicked(ImGuiMouseButton_Right);
            M.dragStart = ImGui::GetIO().MousePos;
        }
    }
    if (M.dragSel) {
        const ImVec2 cur = ImGui::GetIO().MousePos;
        dl->AddRect(M.dragStart, cur, IM_COL32(255, 200, 80, 220));
        dl->AddRectFilled(M.dragStart, cur, IM_COL32(255, 200, 80, 40));
        const bool released = M.dragErase
                                  ? ImGui::IsMouseReleased(ImGuiMouseButton_Right)
                                  : ImGui::IsMouseReleased(ImGuiMouseButton_Left);
        if (released) {
            auto a = chunk_at(M.dragStart);
            auto b = chunk_at(cur);
            const int x0 = (std::min)(a.first, b.first);
            const int x1 = (std::max)(a.first, b.first);
            const int z0 = (std::min)(a.second, b.second);
            const int z1 = (std::max)(a.second, b.second);
            for (const auto& r : M.regions) {
                for (int lz = 0; lz < 32; ++lz)
                    for (int lx = 0; lx < 32; ++lx) {
                        if (!r.present[static_cast<std::size_t>(lz) * 32 + lx])
                            continue;
                        const int cx = r.rx * 32 + lx, cz = r.rz * 32 + lz;
                        if (cx < x0 || cx > x1 || cz < z0 || cz > z1) continue;
                        if (M.dragErase)
                            M.selected.erase({cx, cz});
                        else
                            M.selected.insert({cx, cz});
                    }
            }
            M.dragSel = false;
        }
    }

    dl->PopClipRect();

    // Coordonnees sous le curseur.
    if (hovered && M.loaded) {
        const auto c = chunk_at(ImGui::GetIO().MousePos);
        char buf[96];
        std::snprintf(buf, sizeof(buf), "chunk %d, %d  ·  bloc %d, %d", c.first,
                      c.second, c.first * 16, c.second * 16);
        dl->AddText(ImVec2(p0.x + 8, p1.y - 20),
                    ImGui::ColorConvertFloat4ToU32(kDim), buf);
    }
}

// Suppression effective : regroupee par region pour n'ouvrir chaque fichier
// qu'une fois.
int delete_selection() {
    std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> byRegion;
    for (const auto& [cx, cz] : M.selected) {
        const int rx = static_cast<int>(std::floor(cx / 32.0));
        const int rz = static_cast<int>(std::floor(cz / 32.0));
        byRegion[{rx, rz}].push_back({cx - rx * 32, cz - rz * 32});
    }
    int done = 0;
    for (const auto& [rk, locals] : byRegion) {
        const std::filesystem::path f =
            M.worldPath / "region" /
            ("r." + std::to_string(rk.first) + "." + std::to_string(rk.second) +
             ".mca");
        done += region::clear_chunks(f, locals);
    }
    return done;
}

// --- Operations WorldEdit --------------------------------------------------

// Lance une operation sur un thread de fond. `op` recoit le monde, la
// progression et le drapeau d'annulation, et renvoie son resultat.
void run_we(const std::string& title,
            std::function<worldedit::OpResult(const std::filesystem::path&,
                                              const worldedit::Progress&,
                                              const std::atomic<bool>&)>
                op) {
    {
        std::lock_guard<std::mutex> lk(M.wm);
        if (M.weRunning) return;
        M.weRunning = true;
        M.weResult.clear();
        M.weStatus = title + "...";
    }
    const auto world = M.worldPath;
    tasks::run(title, [world, op](const std::atomic<bool>& cancel,
                                  const std::function<void(const std::string&,
                                                           double)>& report) {
        const auto r = op(
            world,
            [&](const std::string& s) {
                report(s, -1.0);
                std::lock_guard<std::mutex> lk(M.wm);
                M.weStatus = s;
            },
            cancel);
        std::string msg;
        if (!r.error.empty()) {
            msg = r.error;
        } else {
            msg = std::to_string(r.changed) + " bloc(s), " +
                  std::to_string(r.chunks) + " chunk(s).";
            if (r.skipped > 0)
                msg += " " + std::to_string(r.skipped) +
                       " hors des chunks générés (ignorés).";
            if (r.unsupported > 0)
                msg += " " + std::to_string(r.unsupported) +
                       " sans équivalent en 1.12.";
        }
        std::lock_guard<std::mutex> lk(M.wm);
        M.weRunning = false;
        M.weOk = r.error.empty();
        M.weResult = msg;
        M.weStatus.clear();
        M.undoN = M.hist.undo_count();
        M.redoN = M.hist.redo_count();
        M.histKo = M.hist.bytes() / 1024;
        M.clipN = M.clip.blocks.size();
        if (!r.error.empty()) throw std::runtime_error(r.error);
    });
}

void worldedit_panel() {
    // TL_AUTO_WE=1 : deplie le panneau sans clic (capture et test).
    static bool weAuto = std::getenv("TL_AUTO_WE") != nullptr;
    if (weAuto) {
        ImGui::SetNextItemOpen(true, ImGuiCond_Once);
        weAuto = false;
    }
    if (!ImGui::CollapsingHeader(tr("Opérations WorldEdit", "WorldEdit tools")))
        return;

    bool running;
    std::string weStatus, weResult;
    bool weOk;
    std::size_t undoN, redoN, histKo, clipN;
    {
        std::lock_guard<std::mutex> lk(M.wm);
        running = M.weRunning;
        weStatus = M.weStatus;
        weResult = M.weResult;
        weOk = M.weOk;
        undoN = M.undoN;
        redoN = M.redoN;
        histKo = M.histKo;
        clipN = M.clipN;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Double-clic sur un chunk : pos1 (au centre, sur le plus haut bloc). "
           "Maj+double-clic : pos2. Les chunks jamais générés sont ignorés, "
           "jamais créés. Sauvegarde ton monde avant.",
           "Double-click a chunk for pos1 (centre, topmost block). "
           "Shift+double-click for pos2. Chunks never generated are skipped, "
           "never created. Back up your world first."));
    ImGui::PopStyleColor();

    auto pos_row = [&](const char* label, std::optional<worldedit::Pos>& p,
                       const char* id) {
        ImGui::TextUnformatted(label);
        ImGui::SameLine(70.0f);
        int v[3] = {p ? p->x : 0, p ? p->y : 64, p ? p->z : 0};
        ImGui::SetNextItemWidth(230.0f);
        if (ImGui::InputInt3(id, v)) p = worldedit::Pos{v[0], v[1], v[2]};
        ImGui::SameLine();
        if (!p) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("non définie", "unset"));
            ImGui::PopStyleColor();
        } else if (ImGui::SmallButton(
                       (std::string(tr("Effacer", "Clear")) + id).c_str())) {
            p.reset();
        }
    };
    pos_row("pos1", M.pos1, "##we1");
    pos_row("pos2", M.pos2, "##we2");

    const bool haveSel = M.pos1.has_value() && M.pos2.has_value();
    worldedit::Bounds b;
    if (haveSel) b = worldedit::bounds_of(*M.pos1, *M.pos2);
    const std::int64_t vol = haveSel ? b.volume() : 0;
    const bool volOk = haveSel && vol > 0 && vol <= worldedit::kMaxVolume;

    ImGui::PushStyleColor(ImGuiCol_Text, volOk || !haveSel ? kDim : kDanger);
    if (!haveSel)
        ImGui::TextUnformatted(
            tr("Définis pos1 et pos2 pour délimiter un cuboïde.",
               "Set pos1 and pos2 to define a cuboid."));
    else
        ImGui::Text(tr("Sélection : %d x %d x %d = %lld bloc(s)%s",
                       "Selection: %d x %d x %d = %lld block(s)%s"),
                    b.x2 - b.x1 + 1, b.y2 - b.y1 + 1, b.z2 - b.z1 + 1,
                    static_cast<long long>(vol),
                    volOk ? "" : tr(" - au-delà du plafond de 8 000 000",
                                    " - over the 8,000,000 cap"));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::TextUnformatted(tr("Bloc", "Block"));
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("##weset", M.blockSet, sizeof(M.blockSet));
    ImGui::SameLine();
    ImGui::BeginDisabled(running || !volOk);
    if (ImGui::Button("//set")) {
        const std::string name = M.blockSet;
        run_we(tr("Remplissage", "Fill"),
               [b, name](const std::filesystem::path& w,
                         const worldedit::Progress& pr,
                         const std::atomic<bool>& c) {
                   return worldedit::set_region(w, b, name, &M.hist, pr, &c);
               });
    }
    ImGui::EndDisabled();

    ImGui::TextUnformatted(tr("Remplacer", "Replace"));
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##wefrom", M.blockFrom, sizeof(M.blockFrom));
    ImGui::SameLine();
    ImGui::TextUnformatted("->");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("##weto", M.blockTo, sizeof(M.blockTo));
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || !volOk);
    if (ImGui::Button("//replace")) { // « * » dans le champ de gauche = tout
        const std::string from = M.blockFrom, to = M.blockTo;
        run_we(tr("Remplacement", "Replace"),
               [b, from, to](const std::filesystem::path& w,
                             const worldedit::Progress& pr,
                             const std::atomic<bool>& c) {
                   return worldedit::replace_region(w, b, from, to, &M.hist, pr,
                                                    &c);
               });
    }
    ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(
        tr("« * » remplace tout ce qui n'est pas déjà le bloc d'arrivée.",
           "\"*\" replaces everything that is not already the target block."));
    ImGui::PopStyleColor();

    ImGui::Spacing();
    ImGui::BeginDisabled(running || !volOk);
    if (ImGui::Button("//copy")) {
        run_we(tr("Copie", "Copy"),
               [b](const std::filesystem::path& w, const worldedit::Progress& pr,
                   const std::atomic<bool>& c) {
                   return worldedit::copy_region(w, b, M.clip, pr, &c);
               });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || clipN == 0 || !M.pos1.has_value());
    if (ImGui::Button("//paste")) {
        const worldedit::Pos o = *M.pos1;
        run_we(tr("Collage", "Paste"),
               [o](const std::filesystem::path& w, const worldedit::Progress& pr,
                   const std::atomic<bool>& c) {
                   return worldedit::paste(w, M.clip, o, &M.hist, pr, &c);
               });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || undoN == 0);
    if (ImGui::Button("//undo")) {
        run_we(tr("Annulation", "Undo"),
               [](const std::filesystem::path& w, const worldedit::Progress&,
                  const std::atomic<bool>&) { return M.hist.undo(w); });
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || redoN == 0);
    if (ImGui::Button("//redo")) {
        run_we(tr("Rétablissement", "Redo"),
               [](const std::filesystem::path& w, const worldedit::Progress&,
                  const std::atomic<bool>&) { return M.hist.redo(w); });
    }
    ImGui::EndDisabled();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text(tr("Presse-papier : %zu bloc(s)  ·  historique : %zu annulation(s), "
                   "%zu rétablissement(s), %zu Ko",
                   "Clipboard: %zu block(s)  ·  history: %zu undo, %zu redo, %zu KB"),
                clipN, undoN, redoN, histKo);
    ImGui::PopStyleColor();

    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextWrapped("%s", weStatus.c_str());
        ImGui::PopStyleColor();
    } else if (!weResult.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, weOk ? kAccent : kDanger);
        ImGui::TextWrapped("%s", weResult.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::Spacing();
}

} // namespace

void mapeditor_page() {
    // TL_AUTO_MAP=1 : charge le premier monde trouve, sans clic (test).
    static bool autoDone = false;
    if (!autoDone) {
        autoDone = true;
        if (const char* want = std::getenv("TL_AUTO_MAP")) {
            // TL_AUTO_MAP=1 : premier monde trouve. Sinon : filtre sur le nom.
            const std::string filter = (std::strcmp(want, "1") == 0) ? "" : want;
            auto& a = inst_array();
            bool done = false;
            if (a.is_array())
                for (auto& e : a) {
                    if (done || !e.is_object()) continue;
                    const std::string id = e.value("Id", "");
                    if (id.empty()) continue;
                    for (const auto& w : world::list_worlds(
                             DataStore::instancesRoot() / id)) {
                        if (!filter.empty() &&
                            w.name.find(filter) == std::string::npos)
                            continue;
                        M.instId = id;
                        M.worldsDirty = true;
                        load_world(w.path);
                        done = true;
                        break;
                    }
                }
        }
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Édition de carte"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s", tr("Choisis une instance puis un monde. Glisse pour sélectionner des "
                 "chunks (clic droit pour désélectionner), molette pour zoomer, "
                 "bouton du milieu pour déplacer.",
                 "Pick an instance then a world. Drag to select chunks (right "
                 "click to deselect), scroll to zoom, middle button to pan."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // --- selecteurs ---
    auto& arr = inst_array();
    std::string instLabel = tr("Choisir une instance", "Pick an instance");
    if (!M.instId.empty())
        if (const auto* e = find_instance(M.instId)) instLabel = e->value("Name", "?");
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("##mapinst", instLabel.c_str())) {
        if (arr.is_array())
            for (auto& e : arr) {
                if (!e.is_object()) continue;
                const std::string id = e.value("Id", "");
                if (id.empty()) continue;
                if (ImGui::Selectable(e.value("Name", "?").c_str(), id == M.instId)) {
                    M.instId = id;
                    M.worldsDirty = true;
                    load_world({});
                }
            }
        ImGui::EndCombo();
    }
    if (M.worldsDirty) refresh_worlds();

    ImGui::SameLine();
    std::string worldLabel = tr("Choisir un monde", "Pick a world");
    if (!M.worldPath.empty()) worldLabel = M.worldPath.filename().string();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::BeginDisabled(M.worlds.empty());
    if (ImGui::BeginCombo("##mapworld", worldLabel.c_str())) {
        for (const auto& w : M.worlds)
            if (ImGui::Selectable(w.name.c_str(), w.path == M.worldPath))
                load_world(w.path);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // Les deux actions sur leur propre ligne : a 960 px la barre debordait.
    ImGui::Spacing();
    ImGui::BeginDisabled(!M.loaded);
    if (ImGui::Button(tr("Ajuster la vue", "Fit view"), ImVec2(150, 0))) {
        M.zoom = 0.0f; // recalcule l ajustement a la frame suivante
        M.pan = ImVec2(0, 0);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Tout désélectionner", "Deselect all"), ImVec2(180, 0)))
        M.selected.clear();
    ImGui::SameLine();
    ImGui::BeginDisabled(M.selected.empty());
    if (danger_button(tr("Supprimer la sélection", "Delete selection"),
                      ImVec2(200, 0)))
        M.confirmDelete = true;
    ImGui::EndDisabled();
    ImGui::EndDisabled();

    // --- statistiques ---
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (M.loaded)
        ImGui::Text("%d %s  ·  %zu %s  ·  %zu %s  ·  %d x %d %s", M.totalChunks,
                    tr("chunks", "chunks"), M.selected.size(),
                    tr("sélectionné(s)", "selected"), M.regions.size(),
                    tr("région(s)", "region(s)"),
                    M.maxRx - M.minRx + 1, M.maxRz - M.minRz + 1,
                    tr("d'étendue", "extent"));
    else
        ImGui::TextUnformatted(tr("Aucun monde chargé.", "No world loaded."));
    ImGui::PopStyleColor();
    if (!M.status.empty()) {
        ImGui::SameLine();
        ImGui::TextUnformatted(M.status.c_str());
    }
    ImGui::Spacing();

    ImGui::BeginDisabled(!M.loaded);
    worldedit_panel();
    ImGui::EndDisabled();

    draw_canvas();

    // --- confirmation de suppression ---
    if (M.confirmDelete) {
        ImGui::OpenPopup("###mapdel");
        M.confirmDelete = false;
    }
    const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f,
                   ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(c, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(480, 200), ImGuiCond_Appearing);
    const std::string title =
        std::string(tr("Supprimer des chunks", "Delete chunks")) + "###mapdel";
    bool open = true;
    if (ImGui::BeginPopupModal(title.c_str(), &open,
                               ImGuiWindowFlags_NoResize |
                                   ImGuiWindowFlags_NoCollapse)) {
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x);
        ImGui::Text(tr("%zu chunk(s) vont être supprimés définitivement du monde "
                       "« %s ».",
                       "%zu chunk(s) will be permanently deleted from world "
                       "\"%s\"."),
                    M.selected.size(), M.worldPath.filename().string().c_str());
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(
            tr("Cette action est irréversible. Sauvegarde ton monde avant.",
               "This cannot be undone. Back up your world first."));
        ImGui::PopStyleColor();
        ImGui::PopTextWrapPos();

        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - 52.0f);
        if (ImGui::Button(tr("Annuler"), ImVec2(150, 34))) ImGui::CloseCurrentPopup();
        ImGui::SameLine();
        if (danger_button(tr("Supprimer"), ImVec2(150, 34))) {
            const int n = delete_selection();
            M.status = std::to_string(n) + tr(" chunk(s) supprimé(s).",
                                              " chunk(s) deleted.");
            notify_toast(tr("Édition de carte"), M.status);
            const auto path = M.worldPath;
            load_world(path); // relit les en-tetes
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

} // namespace tl::ui

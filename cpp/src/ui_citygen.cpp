#include "ui_internal.hpp"

#include "apptasks.hpp"
#include "citygen.hpp"
#include "region.hpp"
#include "world.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

// ---------------------------------------------------------------------------
// Generateur de ville (portage de la page « Arnis » du C#) : une emprise
// OpenStreetMap est telechargee depuis Overpass, rasterisee en blocs, puis
// posee dans un monde existant.
//
// DIVERGENCE assumee vs C# : le C# annoncait piloter l'outil externe Arnis,
// mais ne le lancait nulle part ; il serialisait lui-meme un NBT en
// PETIT-boutiste, que Minecraft n'aurait jamais pu relire. Ici tout le chemin
// est interne (Overpass -> rasterisation -> region::set_blocks) et valide par
// un aller-retour disque dans les tests.
//
// Le monde doit deja etre genere : un chunk absent n'est pas fabrique.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct CityState {
    char bbox[128] = "2.2900,48.8500,2.3000,48.8600"; // 7e arrondissement
    int baseY = 64;
    int originX = 0;
    int originZ = 0;

    std::string instId;
    std::filesystem::path worldPath;
    std::vector<world::Info> worlds;
    bool worldsDirty = true;

    // Etat partage avec le worker.
    std::mutex m;
    bool running = false;
    std::string status;
    std::string summary;
    bool ok = false;

    // Apercu (rasterisation seule, sans ecriture).
    bool hasPreview = false;
    std::size_t previewBlocks = 0;
    std::size_t previewEntities = 0;
    int previewMinX = 0, previewMaxX = 0, previewMinZ = 0, previewMaxZ = 0;
};
CityState C;

void set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(C.m);
    C.status = s;
}

void refresh_worlds() {
    C.worlds.clear();
    C.worldsDirty = false;
    if (C.instId.empty()) return;
    C.worlds = world::list_worlds(DataStore::instancesRoot() / C.instId);
}

// Telecharge, rasterise, et (si `write`) pose les blocs. Tourne sur un
// thread de fond via tasks::run.
void start_job(bool write) {
    {
        std::lock_guard<std::mutex> lk(C.m);
        if (C.running) return;
        C.running = true;
        C.ok = false;
        C.summary.clear();
        C.status = tr("Interrogation d'Overpass...", "Querying Overpass...");
    }
    const std::string bbox = C.bbox;
    const int baseY = C.baseY;
    const int ox = C.originX;
    const int oz = C.originZ;
    const std::filesystem::path world = C.worldPath;

    tasks::run(
        write ? std::string(tr("Génération de ville", "City generation"))
              : std::string(tr("Aperçu de la ville", "City preview")),
        [bbox, baseY, ox, oz, world, write](
            const std::atomic<bool>& cancel,
            const std::function<void(const std::string&, double)>& report) {
            auto say = [&](const std::string& s) {
                report(s, -1.0);
                set_status(s);
            };

            const auto box = citygen::parse_bbox(bbox);
            if (!box.valid) throw std::runtime_error("Emprise invalide.");

            const auto data = citygen::fetch_osm(box, say, &cancel);
            say("Rasterisation de " + std::to_string(data.entities.size()) +
                " entite(s)...");
            const auto blocks = citygen::rasterize(data, baseY);

            {
                std::lock_guard<std::mutex> lk(C.m);
                C.hasPreview = true;
                C.previewBlocks = blocks.size();
                C.previewEntities = data.entities.size();
                C.previewMinX = C.previewMaxX = 0;
                C.previewMinZ = C.previewMaxZ = 0;
                for (const auto& b : blocks) {
                    C.previewMinX = (std::min)(C.previewMinX, b.x);
                    C.previewMaxX = (std::max)(C.previewMaxX, b.x);
                    C.previewMinZ = (std::min)(C.previewMinZ, b.z);
                    C.previewMaxZ = (std::max)(C.previewMaxZ, b.z);
                }
            }

            if (!write) {
                std::lock_guard<std::mutex> lk(C.m);
                C.running = false;
                C.ok = true;
                C.summary = std::to_string(blocks.size()) + " bloc(s) prets a poser, " +
                            std::to_string(data.entities.size()) + " entite(s).";
                C.status.clear();
                return;
            }

            if (world.empty()) throw std::runtime_error("Aucun monde choisi.");
            const auto r =
                citygen::paste_into_world(world, blocks, ox, oz, say, &cancel);
            if (!r.error.empty()) throw std::runtime_error(r.error);

            std::string s = std::to_string(r.placed) + " bloc(s) pose(s) dans " +
                            std::to_string(r.chunksWritten) + " chunk(s).";
            if (r.chunksMissing > 0)
                s += " " + std::to_string(r.chunksMissing) +
                     " chunk(s) jamais generes ont ete ignores : explore la zone "
                     "en jeu puis relance.";
            if (r.unsupported > 0)
                s += " " + std::to_string(r.unsupported) +
                     " bloc(s) sans equivalent dans ce monde (version <= 1.12).";
            std::lock_guard<std::mutex> lk(C.m);
            C.running = false;
            C.ok = true;
            C.summary = s;
            C.status.clear();
        },
        [](std::exception_ptr ep) {
            std::string msg = "Echec.";
            try {
                if (ep) std::rethrow_exception(ep);
            } catch (const std::exception& e) {
                msg = e.what();
            } catch (...) {
            }
            std::lock_guard<std::mutex> lk(C.m);
            C.running = false;
            C.ok = false;
            C.summary = msg;
            C.status.clear();
        });
}

} // namespace

void citygen_page() {
    // TL_AUTO_CITY=<emprise> : lance un apercu au premier affichage (test
    // reseau reel contre Overpass, sans clic).
    static bool autoDone = false;
    if (!autoDone) {
        autoDone = true;
        if (const char* want = std::getenv("TL_AUTO_CITY")) {
            std::snprintf(C.bbox, sizeof(C.bbox), "%s", want);
            start_job(false);
        }
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Générateur de ville"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Télécharge une zone d'OpenStreetMap et la reconstruit en blocs dans "
           "un monde existant. Les chunks jamais générés sont ignorés : explore "
           "d'abord la zone en jeu. Fais une sauvegarde du monde avant.",
           "Downloads an OpenStreetMap area and rebuilds it as blocks in an "
           "existing world. Chunks that were never generated are skipped: visit "
           "the area in game first. Back up the world beforehand."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    // --- emprise ---
    ImGui::TextUnformatted(tr("Emprise (minLon,minLat,maxLon,maxLat)",
                              "Bounding box (minLon,minLat,maxLon,maxLat)"));
    ImGui::SetNextItemWidth(340.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::InputText("##citybbox", C.bbox, sizeof(C.bbox));
    ImGui::PopStyleColor();
    const auto box = citygen::parse_bbox(C.bbox);
    ImGui::SameLine();
    if (!box.valid) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(tr("emprise invalide", "invalid box"));
        ImGui::PopStyleColor();
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        // ~111 km par degre de latitude, resserre en longitude par cos(lat).
        const double midLat = (box.minLat + box.maxLat) * 0.5;
        const double km = (box.maxLat - box.minLat) * 111.0;
        const double kmx = (box.maxLon - box.minLon) * 111.0 *
                           std::cos(midLat * 3.14159265358979 / 180.0);
        ImGui::Text("~%.2f x %.2f km", kmx, km);
        ImGui::PopStyleColor();
    }

    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt(tr("Altitude de base", "Base height"), &C.baseY);
    C.baseY = std::clamp(C.baseY, -64, 300);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt(tr("Décalage X", "Offset X"), &C.originX, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    ImGui::InputInt(tr("Décalage Z", "Offset Z"), &C.originZ, 0, 0);

    // --- destination ---
    ImGui::Spacing();
    auto& arr = inst_array();
    std::string instLabel = tr("Choisir une instance", "Pick an instance");
    if (!C.instId.empty())
        if (const auto* e = find_instance(C.instId)) instLabel = e->value("Name", "?");
    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::BeginCombo("##cityinst", instLabel.c_str())) {
        if (arr.is_array())
            for (auto& e : arr) {
                if (!e.is_object()) continue;
                const std::string id = e.value("Id", "");
                if (id.empty()) continue;
                if (ImGui::Selectable(e.value("Name", "?").c_str(), id == C.instId)) {
                    C.instId = id;
                    C.worldsDirty = true;
                    C.worldPath.clear();
                }
            }
        ImGui::EndCombo();
    }
    if (C.worldsDirty) refresh_worlds();

    ImGui::SameLine();
    std::string worldLabel = tr("Choisir un monde", "Pick a world");
    if (!C.worldPath.empty()) worldLabel = C.worldPath.filename().string();
    ImGui::SetNextItemWidth(240.0f);
    ImGui::BeginDisabled(C.worlds.empty());
    if (ImGui::BeginCombo("##cityworld", worldLabel.c_str())) {
        for (const auto& w : C.worlds)
            if (ImGui::Selectable(w.name.c_str(), w.path == C.worldPath))
                C.worldPath = w.path;
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();

    // --- actions ---
    bool running;
    std::string status, summary;
    bool ok, hasPreview;
    std::size_t pBlocks, pEntities;
    int pMinX, pMaxX, pMinZ, pMaxZ;
    {
        std::lock_guard<std::mutex> lk(C.m);
        running = C.running;
        status = C.status;
        summary = C.summary;
        ok = C.ok;
        hasPreview = C.hasPreview;
        pBlocks = C.previewBlocks;
        pEntities = C.previewEntities;
        pMinX = C.previewMinX; pMaxX = C.previewMaxX;
        pMinZ = C.previewMinZ; pMaxZ = C.previewMaxZ;
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(running || !box.valid);
    if (ImGui::Button(tr("Aperçu", "Preview"))) start_job(false);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || !box.valid || C.worldPath.empty());
    if (ImGui::Button(tr("Générer dans le monde", "Build into world")))
        start_job(true);
    ImGui::EndDisabled();
    if (C.worldPath.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("(choisis un monde)", "(pick a world)"));
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
    } else if (!summary.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ok ? kAccent : kDanger);
        ImGui::TextWrapped("%s", summary.c_str());
        ImGui::PopStyleColor();
    }

    if (hasPreview) {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        stat_card(std::to_string(pEntities), tr("Entités", "Features"));
        ImGui::SameLine();
        stat_card(std::to_string(pBlocks), tr("Blocs", "Blocks"));
        ImGui::SameLine();
        stat_card(std::to_string(pMaxX - pMinX + 1) + " x " +
                      std::to_string(pMaxZ - pMinZ + 1),
                  tr("Emprise (blocs)", "Extent (blocks)"));
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text(tr("Coins : X %d à %d, Z %d à %d (décalage compris)",
                       "Corners: X %d to %d, Z %d to %d (offset included)"),
                    pMinX + C.originX, pMaxX + C.originX, pMinZ + C.originZ,
                    pMaxZ + C.originZ);
        ImGui::PopStyleColor();
    }
}

} // namespace tl::ui

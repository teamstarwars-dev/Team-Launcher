#include "ui_internal.hpp"

#include "modupdate.hpp"

#include <thread>

// ---------------------------------------------------------------------------
// Phase 5 — « changelog rapide sur mise a jour d'un mod ».
//
// Deux principes ont guide ce panneau :
//
//   - Rien ne part tout seul. La verification interroge le reseau pour
//     chaque projet : la declencher a l'ouverture de l'onglet enverrait
//     des dizaines de requetes a Modrinth chaque fois qu'on vient juste
//     regarder sa liste de mods.
//   - Le changelog est montre AVANT le bouton, pas apres. Mettre un mod a
//     jour au milieu d'une partie en cours casse des mondes ; le texte de
//     l'auteur est ce qui permet de decider.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct Upd {
    std::mutex m;
    std::thread th;
    bool busy = false;
    bool done = false;
    std::string instId;
    modupdate::Result res;
    int progDone = 0, progTotal = 0;
    std::atomic<bool> cancel{false};
    int taskId = 0;
    // Mod en cours d'application, pour griser le bon bouton.
    std::string applying;
    std::string lastError;
};
Upd u;

void reap() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(u.m);
        if (u.busy || !u.th.joinable()) return;
        th = std::move(u.th);
    }
    th.join();
}

void start_check(const std::string& instId, const std::filesystem::path& modsDir,
                 const std::string& loader, const std::string& mcVersion) {
    {
        std::lock_guard<std::mutex> lk(u.m);
        if (u.busy) return;
        u.busy = true;
        u.done = false;
        u.res = modupdate::Result{};
        u.progDone = 0;
        u.progTotal = 0;
        u.lastError.clear();
    }
    reap();
    u.cancel.store(false);
    const int tid = apptasks_begin("Mises à jour des mods", "Analyse...",
                                   &u.cancel);
    std::lock_guard<std::mutex> lk(u.m);
    u.taskId = tid;
    u.th = std::thread([instId, modsDir, loader, mcVersion, tid] {
        auto r = modupdate::check(modsDir, loader, mcVersion, &u.cancel,
                                  [tid](int done, int total) {
                                      std::lock_guard<std::mutex> lk2(u.m);
                                      u.progDone = done;
                                      u.progTotal = total;
                                      tl::tasks::update(
                                          tid,
                                          std::to_string(done) + " / " +
                                              std::to_string(total),
                                          total > 0
                                              ? static_cast<double>(done) / total
                                              : -1.0);
                                  });
        apptasks_end(tid, r.error);
        std::lock_guard<std::mutex> lk2(u.m);
        u.busy = false;
        u.done = true;
        u.instId = instId;
        u.res = std::move(r);
        u.taskId = 0;
    });
}

void start_apply(const modupdate::Update& up) {
    {
        std::lock_guard<std::mutex> lk(u.m);
        if (u.busy) return;
        u.busy = true;
        u.applying = up.file.filename().string();
        u.lastError.clear();
    }
    reap();
    u.cancel.store(false);
    const int tid = apptasks_begin("Mise à jour de " + up.displayName,
                                   "Téléchargement...", &u.cancel);
    std::lock_guard<std::mutex> lk(u.m);
    u.th = std::thread([up, tid] {
        std::string err;
        const bool ok = modupdate::apply(up, &u.cancel, &err);
        apptasks_end(tid, ok ? "" : err);
        std::lock_guard<std::mutex> lk2(u.m);
        u.busy = false;
        u.applying.clear();
        if (!ok) {
            u.lastError = err;
        } else {
            // Le fichier a change : le rapport n'est plus a jour, et
            // l'analyse de compatibilite non plus.
            auto& list = u.res.updates;
            for (auto it = list.begin(); it != list.end(); ++it)
                if (it->file == up.file) {
                    list.erase(it);
                    break;
                }
            ++u.res.upToDate;
        }
    });
}

} // namespace

void modupdate_panel(const nlohmann::json& inst) {
    reap();
    const std::string id = inst.value("Id", "");
    const std::filesystem::path modsDir = DataStore::instancesRoot() / id / "mods";
    const std::string loader = inst.value("Loader", "Vanilla");
    std::string mc = inst.value("McVersion", "");
    if (mc == "latest" || mc == "release" || mc == "snapshot") mc.clear();

    bool busy, done;
    std::string have, applying, lastError;
    int pd, pt;
    modupdate::Result res;
    {
        std::lock_guard<std::mutex> lk(u.m);
        busy = u.busy;
        done = u.done;
        have = u.instId;
        pd = u.progDone;
        pt = u.progTotal;
        applying = u.applying;
        lastError = u.lastError;
        res = u.res;
    }
    const bool current = done && have == id;

    ImGui::Spacing();
    ImGui::BeginDisabled(busy);
    if (ImGui::Button(current ? tr("Revérifier les mises à jour",
                                   "Re-check for updates")
                              : tr("Chercher des mises à jour",
                                   "Check for updates"),
                      ImVec2(260, 30)))
        start_check(id, modsDir, loader, mc);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (busy && pt > 0)
        ImGui::Text(tr("%d / %d mods examinés...", "%d / %d mods checked..."),
                    pd, pt);
    else if (busy)
        ImGui::TextUnformatted(tr("Analyse...", "Checking..."));
    else if (!current)
        ImGui::TextUnformatted(
            tr("Interroge Modrinth, une requête par mod.",
               "Queries Modrinth, one request per mod."));
    ImGui::PopStyleColor();

    if (!lastError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", lastError.c_str());
        ImGui::PopStyleColor();
    }
    if (!current) return;

    if (!res.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", res.error.c_str());
        ImGui::PopStyleColor();
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text(tr("%d à jour, %d mise(s) à jour, %d non reconnu(s) sur Modrinth",
                   "%d up to date, %d update(s), %d not found on Modrinth"),
                res.upToDate, static_cast<int>(res.updates.size()), res.unknown);
    if (res.unknown > 0)
        ImGui::TextWrapped(
            "%s",
            tr("« Non reconnu » ne veut pas dire « à jour » : ces mods "
               "viennent d'ailleurs (CurseForge, site de l'auteur) et n'ont "
               "pas pu être vérifiés.",
               "\"Not found\" does not mean \"up to date\": these mods come "
               "from elsewhere (CurseForge, the author's site) and could not "
               "be checked."));
    ImGui::PopStyleColor();

    for (const auto& up : res.updates) {
        ImGui::PushID(up.file.string().c_str());
        ImGui::Spacing();
        ImGui::TextUnformatted(up.displayName.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("%s  ->  ",
                    up.currentVersion.empty() ? "?" : up.currentVersion.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0x4ADE80));
        ImGui::TextUnformatted(up.newVersion.c_str());
        ImGui::PopStyleColor();

        // Le changelog d'abord, le bouton ensuite.
        if (!up.changelog.empty()) {
            if (ImGui::TreeNode("##chg", "%s", tr("Changelog"))) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                // Texte brut : c'est du Markdown chez l'auteur, on ne le
                // met pas en forme. Le rendre lisible tel quel vaut mieux
                // que de le rendre a moitie.
                ImGui::TextWrapped("%s", up.changelog.c_str());
                ImGui::PopStyleColor();
                ImGui::TreePop();
            }
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("(l'auteur n'a pas écrit de changelog)",
                                      "(the author wrote no changelog)"));
            ImGui::PopStyleColor();
        }

        ImGui::BeginDisabled(busy);
        if (ImGui::SmallButton(applying == up.file.filename().string()
                                   ? tr("Mise à jour...", "Updating...")
                                   : tr("Mettre à jour", "Update"))) {
            start_apply(up);
            modcheck_invalidate();
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
}

void modupdate_stop() {
    u.cancel.store(true);
    reap();
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(u.m);
        th = std::move(u.th);
    }
    if (th.joinable()) th.join();
}

} // namespace tl::ui

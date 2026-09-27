#include "ui_internal.hpp"

#include "curseforge.hpp"
#include "pack_import.hpp"
#include "pack_share.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>

// ---------------------------------------------------------------------------
// Import de modpacks (module 4d) : selection du fichier, worker de fond,
// bandeau de progression, ajout de l'instance au config a la fin.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct PackState {
    std::mutex m;
    std::thread th;
    bool running = false;
    bool done = false;         // resultat pret, a consommer par le main thread
    std::string status;
    packs::Result result;
    std::atomic<bool> cancel{false};
    int taskId = 0; // entree AppTasks de l'operation en cours (sous m)
};
PackState P;

// Cloture la tache AppTasks d'apres le resultat d'un worker de pack.
// Appele PAR LE WORKER : le panneau reste juste meme si l'utilisateur est
// deja sur une autre page (l'UI locale, elle, consomme le resultat plus tard).
void end_pack_task(int tid, bool cancelled, const std::string& error) {
    if (tid <= 0) return;
    if (cancelled) {
        // Annulation locale (bouton de la page, panneau ou shutdown) : le
        // drapeau n'est parfois leve que dans P.cancel, on le propage.
        (void)tl::tasks::cancel(tid);
        apptasks_end(tid);
    } else if (!error.empty()) {
        apptasks_end(tid, error);
    } else {
        apptasks_end(tid);
    }
}

std::optional<std::string> pick_modpack() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = L"Modpacks (*.zip;*.mrpack)\0*.zip;*.mrpack\0"
                      L"Tous les fichiers\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
}

} // namespace

void import_modpack_start(const std::string& path) {
    const std::string title = std::string(tr("Import du modpack", "Modpack import")) +
                              " « " +
                              std::filesystem::path(path).filename().string() +
                              " »";
    int tid = 0;
    {
        std::lock_guard<std::mutex> lk(P.m);
        if (P.running) return;
        if (P.th.joinable()) P.th.join();
        P.running = true;
        P.done = false;
        P.cancel = false;
        P.status = tr("Lecture de l'archive…");
        P.result = packs::Result{};
        // Tache de fond visible dans le panneau (Annuler -> P.cancel).
        tid = P.taskId = apptasks_begin(title, P.status, &P.cancel);
    }
    P.th = std::thread([path, tid] {
        auto progress = [tid](const std::string& s) {
            {
                std::lock_guard<std::mutex> lk(P.m);
                P.status = s;
            }
            tl::tasks::update(tid, s); // miroir vers le panneau
        };
        packs::Result r = packs::import_any(path, progress, P.cancel);
        const bool cancelled = r.cancelled;
        const std::string error = r.error;
        {
            std::lock_guard<std::mutex> lk(P.m);
            P.result = std::move(r);
            P.running = false;
            P.done = true;
        }
        end_pack_task(tid, cancelled, error);
    });
}

void import_modpack_pick() {
    if (auto p = pick_modpack()) import_modpack_start(*p);
}

// --- Partage d'instance (module 4h) ----------------------------------------

namespace {
// Travail de partage : export (vers le presse-papiers) ou import (depuis lui).
enum class ShareJob { None, Export, Import };
ShareJob s_job = ShareJob::None;
std::string s_shareJson;   // resultat d'export, a copier sur le main thread
share::Stats s_shareStats;
share::ImportResult s_shareImport;
bool s_shareReady = false;
} // namespace

void share_instance_start(const nlohmann::json& inst) {
    if (packs_busy()) return;
    const std::string name = inst.value("Name", "?");
    const std::string title =
        std::string(tr("Partage de l'instance", "Instance share")) + " « " + name +
        " »";
    int tid = 0;
    {
        std::lock_guard<std::mutex> lk(P.m);
        P.running = true;
        P.done = false;
        P.cancel = false;
        P.status = tr("Analyse de l'instance…", "Scanning the instance…");
        tid = P.taskId = apptasks_begin(title, P.status, &P.cancel);
        s_job = ShareJob::Export;
        s_shareReady = false;
    }
    const nlohmann::json copy = inst;
    P.th = std::thread([copy, tid] {
        auto progress = [tid](const std::string& s) {
            {
                std::lock_guard<std::mutex> lk(P.m);
                P.status = s;
            }
            tl::tasks::update(tid, s);
        };
        auto r = share::export_pack(copy, progress, P.cancel);
        const bool cancelled = r.cancelled;
        const std::string error = r.error;
        {
            std::lock_guard<std::mutex> lk(P.m);
            s_shareJson = r.pack.is_object() ? share::serialize(r.pack) : std::string();
            s_shareStats = r.stats;
            P.result = packs::Result{};
            P.result.error = r.error;
            P.result.cancelled = r.cancelled;
            s_shareReady = true;
            P.running = false;
            P.done = true;
        }
        end_pack_task(tid, cancelled, error);
    });
}

void import_shared_start(const std::string& text) {
    if (packs_busy()) return;
    const std::string title =
        tr("Import d'un pack partagé", "Shared pack import");
    int tid = 0;
    {
        std::lock_guard<std::mutex> lk(P.m);
        P.running = true;
        P.done = false;
        P.cancel = false;
        P.status = tr("Lecture du pack partagé…", "Reading the shared pack…");
        tid = P.taskId = apptasks_begin(title, P.status, &P.cancel);
        s_job = ShareJob::Import;
        s_shareReady = false;
    }
    P.th = std::thread([text, tid] {
        auto progress = [tid](const std::string& s) {
            {
                std::lock_guard<std::mutex> lk(P.m);
                P.status = s;
            }
            tl::tasks::update(tid, s);
        };
        auto r = share::import_pack(text, progress, P.cancel);
        const bool cancelled = r.cancelled;
        const std::string error = r.error;
        {
            std::lock_guard<std::mutex> lk(P.m);
            s_shareImport = std::move(r);
            s_shareReady = true;
            P.running = false;
            P.done = true;
        }
        end_pack_task(tid, cancelled, error);
    });
}

void import_shared_from_clipboard() {
    const char* clip = ImGui::GetClipboardText();
    const std::string text = clip ? trimmed(clip) : std::string();
    if (text.empty()) {
        notify_toast(tr("Presse-papiers vide"),
                     tr("Copie d'abord le pack partagé par ton ami.",
                        "Copy your friend's shared pack first."));
        return;
    }
    if (!share::looks_like_pack(text)) {
        notify_toast(tr("Pack non reconnu"),
                     tr("Le presse-papiers ne contient pas un pack Team Launcher.",
                        "The clipboard does not contain a Team Launcher pack."));
        return;
    }
    import_shared_start(text);
}

// Bandeau + consommation du resultat. Appele une fois par frame.
void packs_frame() {
    bool running = false, done = false;
    std::string status;
    int taskId = 0;
    {
        std::lock_guard<std::mutex> lk(P.m);
        running = P.running;
        done = P.done;
        status = P.status;
        taskId = P.taskId;
    }

    if (running) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::BeginChild("##packprog", ImVec2(0, 52), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(status.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
        if (ImGui::Button(tr("Annuler"), ImVec2(90, 0))) {
            P.cancel = true;
            // Meme annulation que celle du panneau de taches (etat coherent
            // des deux cotes — le worker clot la tache en « Annulee »).
            if (taskId) (void)tl::tasks::cancel(taskId);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        return;
    }

    if (!done) return;

    // --- Resultat : ajout de l'instance sur le main thread ---
    packs::Result r;
    ShareJob job = ShareJob::None;
    std::string shareJson;
    share::Stats stats;
    share::ImportResult imported;
    {
        std::lock_guard<std::mutex> lk(P.m);
        r = std::move(P.result);
        job = s_job;
        s_job = ShareJob::None;
        shareJson = std::move(s_shareJson);
        stats = s_shareStats;
        imported = std::move(s_shareImport);
        s_shareImport = share::ImportResult{};
        P.done = false;
    }
    if (P.th.joinable()) P.th.join();

    // --- Export d'un pack : copie du descriptif dans le presse-papiers ---
    if (job == ShareJob::Export) {
        if (r.cancelled) {
            notify_toast(tr("Partage annulé"), tr("Rien n'a été copié.",
                                                  "Nothing was copied."));
            return;
        }
        if (!r.error.empty() || shareJson.empty()) {
            notify_toast(tr("Partage impossible"),
                         r.error.empty() ? tr("Analyse de l'instance échouée.",
                                              "Instance scan failed.")
                                         : r.error);
            return;
        }
        ImGui::SetClipboardText(shareJson.c_str());
        const int known = stats.recognizedMods + stats.recognizedShaders;
        const int all = stats.mods + stats.shaders;
        std::string msg = tr("Pack copié : ", "Pack copied: ") +
                          std::to_string(known) + "/" + std::to_string(all) +
                          tr(" fichier(s) reconnus sur Modrinth. Colle-le à tes amis.",
                             " file(s) recognised on Modrinth. Paste it to your friends.");
        if (known < all)
            msg += tr("\nLes autres devront être fournis à la main.",
                      "\nThe rest will have to be shared manually.");
        notify_toast(tr("Pack partagé"), msg);
        return;
    }

    // --- Import d'un pack partage ---
    if (job == ShareJob::Import) {
        if (imported.cancelled) {
            notify_toast(tr("Import annulé"), tr("Le pack n'a pas été importé.",
                                                 "The pack was not imported."));
            return;
        }
        if (!imported.error.empty()) {
            notify_toast(tr("Erreur d'import"), imported.error);
            return;
        }
        if (!imported.instance.is_object()) return;
        const std::string name = imported.instance.value("Name", "");
        inst_array().push_back(imported.instance);
        DataStore::save();
        g.countsDirty = true;
        std::string msg = "« " + name + " »" +
                          tr(" importé : ", " imported: ") +
                          std::to_string(imported.downloaded) +
                          tr(" fichier(s).", " file(s).");
        if (imported.failed > 0)
            msg += " " + std::to_string(imported.failed) +
                   tr(" non résolu(s) sur Modrinth.",
                      " could not be resolved on Modrinth.");
        notify_toast(tr("Pack importé"), msg);
        return;
    }

    if (r.cancelled) {
        notify_toast(tr("Import annulé"), tr("Le modpack n'a pas été importé.",
                                             "The modpack was not imported."));
        return;
    }
    if (!r.error.empty()) {
        notify_toast(tr("Erreur d'import"), r.error);
        return;
    }
    if (!r.instance.is_object()) return;

    const std::string name = r.instance.value("Name", "");
    inst_array().push_back(r.instance);
    DataStore::save();
    g.countsDirty = true;

    std::string msg = tr("« ", "\"") + name + tr(" » importé : ", "\" imported: ") +
                      std::to_string(r.downloaded) +
                      tr(" fichier(s).", " file(s).");
    if (r.failed > 0)
        msg += " " + std::to_string(r.failed) + tr(" échec(s) — voir launcher.log.",
                                                   " failure(s) - see launcher.log.");
    notify_toast(tr("Modpack importé"), msg);
}

void packs_stop() {
    P.cancel = true;
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(P.m);
        th = std::move(P.th);
    }
    if (th.joinable()) th.join();
}

bool packs_busy() {
    std::lock_guard<std::mutex> lk(P.m);
    return P.running;
}

} // namespace tl::ui

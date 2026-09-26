#include "ui_internal.hpp"

#include "curseforge.hpp"
#include "pack_import.hpp"

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
};
PackState P;

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
    {
        std::lock_guard<std::mutex> lk(P.m);
        if (P.running) return;
        if (P.th.joinable()) P.th.join();
        P.running = true;
        P.done = false;
        P.cancel = false;
        P.status = tr("Lecture de l'archive…");
        P.result = packs::Result{};
    }
    P.th = std::thread([path] {
        auto progress = [](const std::string& s) {
            std::lock_guard<std::mutex> lk(P.m);
            P.status = s;
        };
        packs::Result r = packs::import_any(path, progress, P.cancel);
        std::lock_guard<std::mutex> lk(P.m);
        P.result = std::move(r);
        P.running = false;
        P.done = true;
    });
}

void import_modpack_pick() {
    if (auto p = pick_modpack()) import_modpack_start(*p);
}

// Bandeau + consommation du resultat. Appele une fois par frame.
void packs_frame() {
    bool running = false, done = false;
    std::string status;
    {
        std::lock_guard<std::mutex> lk(P.m);
        running = P.running;
        done = P.done;
        status = P.status;
    }

    if (running) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
        ImGui::BeginChild("##packprog", ImVec2(0, 52), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(status.c_str());
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - 90.0f);
        if (ImGui::Button(tr("Annuler"), ImVec2(90, 0))) P.cancel = true;
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        return;
    }

    if (!done) return;

    // --- Resultat : ajout de l'instance sur le main thread ---
    packs::Result r;
    {
        std::lock_guard<std::mutex> lk(P.m);
        r = std::move(P.result);
        P.done = false;
    }
    if (P.th.joinable()) P.th.join();

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

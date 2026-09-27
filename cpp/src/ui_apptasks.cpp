#include "ui_internal.hpp"

#include "apptasks.hpp"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace tl::ui {

// ---------------------------------------------------------------------------
// Panneau des taches de fond (portage du panneau MainForm C# nourri par
// AppTasks) : liste des taches avec barres de progression et bouton Annuler
// par tache. ImGui uniquement — la logique vit dans tl::tasks.
// ---------------------------------------------------------------------------

namespace {

// Relais d'annulation panneau -> pages (module « rebranchement AppTasks »).
// Le bouton Annuler du panneau n'ecrit que dans le registre : chaque frame,
// apptasks_frame() recopie ce drapeau dans l'atomic local que la page sonde
// deja dans son worker. Le pointeur est toujours une variable statique (g.,
// P., E., S...) ou un shared_ptr garde par la tache en attente d'execution.
std::mutex g_bindM;
std::vector<std::pair<int, std::atomic<bool>*>> g_bind;

} // namespace

int apptasks_begin(const std::string& title, const std::string& status,
                   std::atomic<bool>* localCancel) {
    const int id = tl::tasks::create(title, status);
    if (localCancel) {
        std::lock_guard<std::mutex> lk(g_bindM);
        g_bind.emplace_back(id, localCancel);
    }
    return id;
}

void apptasks_end(int id, const std::string& message) {
    if (id <= 0) return;
    {
        std::lock_guard<std::mutex> lk(g_bindM);
        for (auto it = g_bind.begin(); it != g_bind.end(); ++it) {
            if (it->first != id) continue;
            // Annulation demandee par le panneau mais pas encore relayee
            // (frame en cours) : on la propage avant de detacher le lien.
            if (tl::tasks::cancelled(id) && it->second)
                it->second->store(true);
            g_bind.erase(it);
            break;
        }
    }
    // Sur une tache deja annulee (panneau), finish/fail sont sans effet :
    // l'entree reste « Annulee », comme voulu.
    if (!message.empty())
        (void)tl::tasks::fail(id, message);
    else
        (void)tl::tasks::finish(id);
}

namespace {

// Une fois par frame (thread UI) : annulation panneau -> atomique local,
// et purge des liens dont la tache a disparu du registre (clear_finished).
void relay_cancels() {
    std::lock_guard<std::mutex> lk(g_bindM);
    if (g_bind.empty()) return;
    const std::vector<tl::tasks::Info> snap = tl::tasks::snapshot();
    for (auto it = g_bind.begin(); it != g_bind.end();) {
        const tl::tasks::Info* f = nullptr;
        for (const auto& i : snap)
            if (i.id == it->first) {
                f = &i;
                break;
            }
        if (!f) {
            it = g_bind.erase(it);
            continue;
        }
        if (f->cancelRequested && it->second) it->second->store(true);
        ++it;
    }
}

} // namespace

const char* apptasks_state_label(tl::tasks::State s) {
    using tl::tasks::State;
    switch (s) {
    case State::Running: return tr("En cours", "Running");
    case State::Done: return tr("Terminée", "Done");
    case State::Failed: return tr("Échouée", "Failed");
    case State::Cancelled: return tr("Annulée", "Cancelled");
    }
    return "";
}

void apptasks_panel() {
    using tl::tasks::State;
    std::vector<tl::tasks::Info> items = tl::tasks::snapshot();

    std::size_t running = 0;
    for (const auto& t : items)
        if (t.state == State::Running) ++running;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::BeginChild("##apptasks", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
    if (fBig) ImGui::PushFont(fBig);
    if (running > 0)
        ImGui::TextUnformatted((std::string(tr("Taches de fond", "Background tasks")) +
                                " (" + std::to_string(running) + ")")
                                   .c_str());
    else
        ImGui::TextUnformatted(tr("Taches de fond", "Background tasks"));
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    if (items.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped(
            "%s", tr("Aucune tache en cours. Les imports et installations "
                     "apparaitront ici.",
                     "No task running. Imports and installs will show up here."));
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        return;
    }

    for (const auto& t : items) {
        ImGui::PushID(t.id);
        // Titre + etat (traduction a l'affichage : les pages deposent des
        // chaines source, ecrites depuis le thread UI ou un worker).
        const std::string title = tr(t.title);
        ImGui::TextUnformatted(title.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (fSmall) ImGui::PushFont(fSmall);
        ImGui::TextUnformatted(
            (std::string("- ") + apptasks_state_label(t.state)).c_str());
        if (fSmall) ImGui::PopFont();
        ImGui::PopStyleColor();

        // Statut detaille
        if (!t.status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (fSmall) ImGui::PushFont(fSmall);
            const std::string status = tr(t.status);
            ImGui::TextWrapped("%s", status.c_str());
            if (fSmall) ImGui::PopFont();
            ImGui::PopStyleColor();
        }

        // Progression : reelle 0..1, indeterminee si < 0.
        if (t.state == State::Running) {
            if (t.progress >= 0.0) {
                char overlay[16];
                std::snprintf(overlay, sizeof(overlay), "%d %%",
                              static_cast<int>(t.progress * 100.0));
                ImGui::ProgressBar(static_cast<float>(t.progress),
                                   ImVec2(-100.0f, 0.0f), overlay);
            } else {
                const float tt = static_cast<float>(ImGui::GetTime());
                const float frac =
                    0.2f + 0.6f * (0.5f + 0.5f * std::sin(tt * 2.0f));
                ImGui::ProgressBar(frac, ImVec2(-100.0f, 0.0f), "");
            }
            ImGui::SameLine();
            if (danger_button(tr("Annuler", "Cancel"), ImVec2(90, 0)))
                tl::tasks::cancel(t.id);
        } else {
            // Terminale : barre pleine / message conserve, pas de bouton Annuler.
            const float frac = (t.state == State::Done) ? 1.0f : 0.0f;
            ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f), "");
        }
        ImGui::Spacing();
        ImGui::PopID();
    }

    // Purge des terminees (le registre conserve les etats terminaux pour
    // affichage, contrairement au C# qui retirait l'entree aussitot).
    bool anyFinished = false;
    for (const auto& t : items)
        if (t.state != State::Running) {
            anyFinished = true;
            break;
        }
    ImGui::BeginDisabled(!anyFinished);
    if (accent_button(tr("Effacer les terminees", "Clear finished"),
                      ImVec2(220, 0)))
        tl::tasks::clear_finished();
    ImGui::EndDisabled();

    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

void apptasks_frame() {
    // Relais panneau -> atomiques locales des pages : meme si aucune tache
    // n'est affichee (compteur a 0 apres purge), les liens doivent etre
    // epures avant le rendu.
    relay_cancels();
    if (tl::tasks::count() == 0) return;
    apptasks_panel();
}

} // namespace tl::ui

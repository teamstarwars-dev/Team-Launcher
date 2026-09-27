#pragma once

// Portage de AppTasks.cs — registre thread-safe des taches de fond visibles
// dans le panneau de l'UI (imports, installations...), chacune annulable.
//
// Logique pure SANS ImGui : mutex + atomiques uniquement. Le panneau ImGui
// vit dans ui_apptasks.cpp et ne fait que snapshot() + cancel().
// Windows uniquement, C++20.

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace tl::tasks {

enum class State {
    Running,   // en cours
    Done,      // terminee (succes)
    Failed,    // echouee (message dans status)
    Cancelled, // annulee (drapeau cancel demande, puis acte)
};

struct Info {
    int id = 0;
    std::string title;
    std::string status;
    double progress = 0.0; // 0..1, -1 = indeterminee (barre animee cote UI)
    State state = State::Running;
    bool cancelRequested = false; // copie du drapeau au moment du snapshot
    std::int64_t startedUnix = 0; // secondes system_clock (horodatage)
};

// Cree une tache a l'etat Running, progression 0. Retourne son id (> 0).
int create(const std::string& title, const std::string& status = "");

// Met a jour libelle de statut et/ou progression (progress < 0 = inchangee,
// sinon clamp 0..1). Sans effet si id inconnu ou tache terminale.
void update(int id, const std::string& status, double progress = -1.0);

// Transitions terminales. finish() passe Done (progress 1 sauf si deja
// renseignee), fail(msg) passe Failed avec le message, cancel() leve le
// drapeau d'annulation et passe Cancelled si encore Running.
// Retourne false si id inconnu.
bool finish(int id);
bool fail(int id, const std::string& message);
bool cancel(int id);

// Drapeau d'annulation brut (pour les workers qui sondent en boucle).
// false si id inconnu.
bool cancelled(int id);

// Copie de toutes les taches (terminales incluses) pour l'UI.
std::vector<Info> snapshot();

// Nombre total / nombre en cours.
std::size_t count();
std::size_t count_running();

// Supprime les taches terminales (Done/Failed/Cancelled). Appele par le
// bouton "Effacer" du panneau.
void clear_finished();

// Compteur de version : incremente a chaque mutation (equivalent de
// l'evenement C# Changed, en version polling compatible ImGui).
std::uint64_t version();

// Remet le registre a zero (tests uniquement).
void reset_for_tests();

// Equivalent de AppTasks.Run : cree l'entree, execute work sur un thread
// de fond, marque Done/Failed/Cancelled a la fin (contrairement au C# qui
// retirait l'entree, on la conserve pour affichage — voir clear_finished).
// set_status(s, p) met a jour le libelle (+ progression optionnelle).
// on_error recoit l'exception en cas d'echec.
std::future<void> run(
    std::string title,
    std::function<void(const std::atomic<bool>& cancel,
                       const std::function<void(const std::string&, double)>& set_status)>
        work,
    std::function<void(std::exception_ptr)> on_error = {});

} // namespace tl::tasks

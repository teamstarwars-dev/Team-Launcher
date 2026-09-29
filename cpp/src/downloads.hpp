#pragma once

// File de telechargements du launcher.
//
// Jusqu'ici chaque appelant (mods, modpacks, skins, jeu, mises a jour)
// lancait son `http::get_to_file` dans son coin. Trois consequences :
//
//  1. **Rien ne limitait le parallelisme.** Installer un modpack de 150 mods
//     ouvrait autant de connexions que de threads disponibles. Le reglage
//     `maxDownloads` existait dans la configuration mais n'etait lu nulle
//     part.
//  2. **Aucune vue d'ensemble.** Impossible de savoir ce qui telechargeait,
//     a quelle vitesse, ni de reprendre ce qui avait echoue.
//  3. **Annulation par appelant**, chacun avec son propre drapeau.
//
// Ce module centralise : une file, un nombre de travailleurs borne par le
// reglage, un etat consultable, l'annulation et la relance par element.
//
// La fonction de recuperation est injectable (`set_fetcher`), ce qui permet
// de tester toute la mecanique de file — limite de parallelisme, annulation,
// relance, compteurs — sans ouvrir une seule connexion reseau.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tl::downloads {

enum class State {
    Queued,    // en attente d'un travailleur libre
    Running,   // en cours
    Done,      // termine
    Failed,    // echoue (message dans `error`)
    Cancelled, // annule par l'utilisateur
};

struct Item {
    int id = 0;
    std::string label;  // ce que voit l'utilisateur (« Sodium 0.6.0 »)
    std::string url;    // URL REDIGEE : jamais de secret dans l'interface
    std::filesystem::path dest;
    long long done = 0;    // octets recus
    long long total = -1;  // taille annoncee, -1 si inconnue
    double speedBps = 0.0; // moyenne glissante
    State state = State::Queued;
    std::string error;
    std::int64_t queuedUnix = 0;
    std::int64_t endedUnix = 0;

    bool active() const { return state == State::Queued || state == State::Running; }
    // 0..1, ou -1 quand la taille totale est inconnue (barre animee).
    double progress() const {
        if (state == State::Done) return 1.0;
        if (total <= 0) return -1.0;
        return static_cast<double>(done) / static_cast<double>(total);
    }
};

// Ajoute un telechargement. Retourne son identifiant (> 0), ou 0 si l'URL
// ou la destination sont vides.
int enqueue(const std::string& label, const std::string& url,
            const std::filesystem::path& dest);

// Copie de l'etat pour l'interface, du plus recent au plus ancien.
std::vector<Item> snapshot();
std::size_t count_active();
std::size_t count_finished();

// Annule un element (en attente ou en cours) ; sans effet s'il est termine.
bool cancel(int id);
void cancel_all();

// Relance un element echoue ou annule. false si l'identifiant est inconnu
// ou si l'element n'est pas dans un etat relancable.
bool retry(int id);

// Retire les elements termines (Done/Failed/Cancelled).
void clear_finished();

// Nombre de telechargements simultanes, borne a [1, 20] comme le reglage.
void set_limit(int n);
int limit();

// Incremente a chaque mutation : l'interface ne se redessine que si besoin.
std::uint64_t version();

// Attend que la file soit vide (tests, et arret propre du launcher).
// timeoutMs < 0 = infini. false si le delai expire.
bool wait_idle(int timeoutMs);

// Arrete les travailleurs (appele a la fermeture).
void shutdown();

// --- Injection pour les tests ----------------------------------------------

using ProgressFn = std::function<void(long long done, long long total)>;
using Fetcher = std::function<bool(const std::string& url,
                                   const std::filesystem::path& dest,
                                   const ProgressFn& progress,
                                   const std::atomic<bool>& cancel,
                                   std::string* errOut)>;

// Remplace le transport. Par defaut : http::get_to_file.
void set_fetcher(Fetcher f);
void reset_for_tests();

} // namespace tl::downloads

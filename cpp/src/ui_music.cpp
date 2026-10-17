#include "ui_internal.hpp"

#include "apptasks.hpp"
#include "sbmusic.hpp"
#include "sbmusic_api.hpp"
#include "sbmusic_player.hpp"

#include <atomic>
#include <cstdio>
#include <future>
#include <mutex>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Page Musique (19) — SB'Music.
//
// Phase 5 : connexion OAuth (flux reel), recherche, bibliotheque et playlists.
// Phase 4 : lecture audio (Ogg/Opus via le lecteur sbmusic_player) — le
// telechargement du flux bloque, il tourne donc sur un worker comme le reste.
//
// Toutes les requetes reseau (login, search, library, playlists, lecture)
// tournent sur des taches de fond ; l'UI ne lit que l'etat partage (sous
// verrou) et l'etat du lecteur (atomiques / verrou interne).
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

enum class Tab { Search, Library, Playlists };

struct MusicState {
    std::mutex m;
    // Resultats courants (titres) + playlists.
    std::vector<sbmusic::Track> tracks;
    std::vector<sbmusic::Playlist> playlists;
    bool loading = false;
    std::uint64_t gen = 0; // invalide les resultats d'une requete depassee

    // Connexion en cours.
    bool loggingIn = false;
    bool loginJustDone = false;
    std::string loginError;

    // Lecture en cours (miroir de l'etat du lecteur, pour les boutons).
    std::string playingId;   // id du titre charge (vide = aucun)
    std::string playingName; // titre affiche dans la barre de lecture
    bool playLoading = false; // telechargement du flux en cours
    std::string playError;

    std::future<void> job; // garde la tache en vie (sinon le future bloque)
    Tab tab = Tab::Search;
    char query[128] = "";
    bool inited = false;
};
MusicState S;

// Lance une recherche de titres sur un worker.
void run_tracks(std::vector<sbmusic::Track> (*fetch)(), const char* title) {
    std::uint64_t myGen;
    {
        std::lock_guard<std::mutex> lk(S.m);
        S.loading = true;
        myGen = ++S.gen;
    }
    S.job = tasks::run(
        title,
        [fetch, myGen](const std::atomic<bool>&,
                       const std::function<void(const std::string&, double)>&) {
            auto r = fetch();
            std::lock_guard<std::mutex> lk(S.m);
            if (myGen != S.gen) return; // une requete plus recente a pris la main
            S.tracks = std::move(r);
            S.loading = false;
        });
}

// Variantes capturant des parametres (recherche, playlist) : wrappers statiques.
std::string g_query;
std::string g_playlistId;
std::vector<sbmusic::Track> fetch_search() { return sbmusic::search(g_query, 30); }
std::vector<sbmusic::Track> fetch_library() { return sbmusic::my_tracks(50); }
std::vector<sbmusic::Track> fetch_playlist() {
    return sbmusic::playlist_tracks(g_playlistId);
}

void load_playlists() {
    std::uint64_t myGen;
    {
        std::lock_guard<std::mutex> lk(S.m);
        S.loading = true;
        myGen = ++S.gen;
    }
    S.job = tasks::run("SB'Music : playlists",
                       [myGen](const std::atomic<bool>&,
                               const std::function<void(const std::string&, double)>&) {
                           auto r = sbmusic::my_playlists();
                           std::lock_guard<std::mutex> lk(S.m);
                           if (myGen != S.gen) return;
                           S.playlists = std::move(r);
                           S.loading = false;
                       });
}

void start_login() {
    {
        std::lock_guard<std::mutex> lk(S.m);
        if (S.loggingIn) return;
        S.loggingIn = true;
        S.loginError.clear();
    }
    S.job = tasks::run(
        "Connexion SB'Music",
        [](const std::atomic<bool>&,
           const std::function<void(const std::string&, double)>&) {
            std::string err;
            const bool ok = sbmusic::login(&err);
            std::lock_guard<std::mutex> lk(S.m);
            S.loggingIn = false;
            S.loginJustDone = ok;
            if (!ok) S.loginError = err.empty() ? "Connexion annulée." : err;
        });
}

std::string fmt_duration(int sec) {
    if (sec <= 0) return "";
    char b[16];
    std::snprintf(b, sizeof(b), "%d:%02d", sec / 60, sec % 60);
    return b;
}

// Telecharge le flux d'un titre puis le joue (le download bloque : worker).
void play_track(const sbmusic::Track& t) {
    {
        std::lock_guard<std::mutex> lk(S.m);
        if (S.playLoading) return;
        S.playLoading = true;
        S.playError.clear();
    }
    S.job = tasks::run(
        "SB'Music : lecture",
        [t](const std::atomic<bool>&,
            const std::function<void(const std::string&, double)>&) {
            std::string err;
            const bool ok = sbmusic::play(t.id, &err);
            std::lock_guard<std::mutex> lk(S.m);
            S.playLoading = false;
            if (ok) {
                S.playingId = t.id;
                S.playingName = t.title;
            } else {
                S.playError = err.empty() ? "Lecture impossible." : err;
            }
        });
}

void tracks_list() {
    std::vector<sbmusic::Track> copy;
    bool loading;
    std::string playingId;
    {
        std::lock_guard<std::mutex> lk(S.m);
        copy = S.tracks;
        loading = S.loading;
        playingId = S.playingId;
    }
    if (loading && copy.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Chargement…"));
        ImGui::PopStyleColor();
        return;
    }
    if (copy.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Aucun titre."));
        ImGui::PopStyleColor();
        return;
    }
    for (const auto& t : copy) {
        ImGui::PushID(t.id.c_str());
        // Bouton lecture : met le titre en lecture (ou le repart s'il est en
        // pause, le meme bouton sert de reprise dans la barre du bas).
        const bool current = (!playingId.empty() && playingId == t.id &&
                              sbmusic::has_track());
        ImGui::BeginDisabled(S.playLoading && !current);
        if (ImGui::Button(current && sbmusic::is_playing() ? "▶" : "►",
                          ImVec2(32, 0))) {
            if (current)
                sbmusic::toggle_pause();
            else
                play_track(t);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (current) {
            ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
            ImGui::TextUnformatted(t.title.c_str());
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(t.title.c_str());
        }
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        std::string meta = " — " + t.artist;
        const std::string d = fmt_duration(t.duration);
        if (!d.empty()) meta += "  (" + d + ")";
        ImGui::TextUnformatted(meta.c_str());
        ImGui::PopStyleColor();
        ImGui::PopID();
    }
}

// Barre de lecture fixee sous la liste : titre courant, position, pause,
// arret. n'affiche que si un titre est charge.
void player_bar() {
    if (!sbmusic::has_track()) return;
    std::string name, err;
    bool loading;
    {
        std::lock_guard<std::mutex> lk(S.m);
        name = S.playingName;
        err = S.playError;
        loading = S.playLoading;
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (loading) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted("Chargement du flux…");
        ImGui::PopStyleColor();
        return;
    }
    const bool playing = sbmusic::is_playing();
    if (ImGui::Button(playing ? "⏸" : "▶")) sbmusic::toggle_pause();
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Arrêter"))) {
        sbmusic::stop();
        std::lock_guard<std::mutex> lk(S.m);
        S.playingId.clear();
        S.playingName.clear();
    }
    ImGui::SameLine();
    const double pos = sbmusic::position_sec();
    const double dur = sbmusic::duration_sec();
    const std::string clock =
        fmt_duration(static_cast<int>(pos)) + " / " + fmt_duration(static_cast<int>(dur));
    ImGui::Text("%s  —  %s", clock.c_str(), name.c_str());
    if (!err.empty()) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextUnformatted(err.c_str());
        ImGui::PopStyleColor();
    }
}

} // namespace

void music_page() {
    if (!S.inited) {
        S.inited = true;
    }

    // Transition de connexion : au succes, on charge la bibliotheque.
    {
        std::lock_guard<std::mutex> lk(S.m);
        if (S.loginJustDone) {
            S.loginJustDone = false;
            S.tab = Tab::Library;
        }
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted("SB'Music");
    if (fBig) ImGui::PopFont();
    ImGui::Spacing();

    // --- Non connecte : bouton de connexion ---
    if (!sbmusic::is_logged_in()) {
        bool loggingIn;
        std::string err;
        {
            std::lock_guard<std::mutex> lk(S.m);
            loggingIn = S.loggingIn;
            err = S.loginError;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::PushTextWrapPos(560);
        ImGui::TextUnformatted(
            tr("Connecte ton compte SB'Music pour chercher, parcourir ta "
               "bibliothèque et tes playlists. Une page s'ouvrira dans ton "
               "navigateur pour autoriser le launcher."));
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        ImGui::Spacing();
        ImGui::BeginDisabled(loggingIn);
        if (accent_button(loggingIn ? tr("Connexion en cours…")
                                    : tr("Se connecter à SB'Music"),
                          ImVec2(260, 44)))
            start_login();
        ImGui::EndDisabled();
        if (!err.empty()) {
            ImGui::Spacing();
            ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }
        return;
    }

    // --- Connecte : onglets + contenu ---
    if (ImGui::Button(tr("Rechercher"))) {
        std::lock_guard<std::mutex> lk(S.m);
        S.tab = Tab::Search;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Ma bibliothèque"))) {
        S.tab = Tab::Library;
        run_tracks(&fetch_library, "SB'Music : bibliothèque");
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Playlists"))) {
        S.tab = Tab::Playlists;
        load_playlists();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Déconnexion"))) {
        sbmusic::stop();
        sbmusic::logout();
        std::lock_guard<std::mutex> lk(S.m);
        S.tracks.clear();
        S.playlists.clear();
        S.playingId.clear();
        S.playingName.clear();
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    Tab tab;
    {
        std::lock_guard<std::mutex> lk(S.m);
        tab = S.tab;
    }

    if (tab == Tab::Search) {
        ImGui::SetNextItemWidth(360);
        const bool go = ImGui::InputTextWithHint(
            "##q", tr("Rechercher un titre, un artiste…"), S.query,
            sizeof(S.query), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if ((ImGui::Button(tr("Chercher")) || go) && S.query[0]) {
            g_query = S.query;
            run_tracks(&fetch_search, "SB'Music : recherche");
        }
        ImGui::Spacing();
        tracks_list();
    } else if (tab == Tab::Library) {
        tracks_list();
    } else { // Playlists
        std::vector<sbmusic::Playlist> pls;
        bool loading;
        {
            std::lock_guard<std::mutex> lk(S.m);
            pls = S.playlists;
            loading = S.loading;
        }
        if (loading && pls.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("Chargement…"));
            ImGui::PopStyleColor();
        } else if (pls.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr("Aucune playlist."));
            ImGui::PopStyleColor();
        } else {
            for (const auto& p : pls) {
                ImGui::PushID(p.id.c_str());
                std::string lbl = p.name;
                if (p.trackCount > 0)
                    lbl += "  (" + std::to_string(p.trackCount) + ")";
                if (ImGui::Selectable(lbl.c_str())) {
                    g_playlistId = p.id;
                    S.tab = Tab::Search; // reutilise la liste de titres
                    run_tracks(&fetch_playlist, "SB'Music : playlist");
                }
                ImGui::PopID();
            }
        }
    }

    player_bar();
}

} // namespace tl::ui

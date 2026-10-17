#pragma once

// Client de l'API REST SB'Music v1 (https://dev.sb-music.com/docs).
// Toutes les requetes portent l'en-tete Authorization: Bearer <access_token>
// (jeton OAuth, voir sbmusic.hpp). Le parsing est defensif : une reponse
// inattendue ne leve jamais, elle rend un resultat vide.

#include <string>
#include <vector>

namespace tl::sbmusic {

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string thumbnail; // URL de la pochette ("" si absente)
    int duration = 0;      // secondes
};

struct Playlist {
    std::string id;
    std::string name;
    int trackCount = 0;
};

// Recherche de titres (type=track). {} si non connecte ou en cas d'echec.
std::vector<Track> search(const std::string& query, int limit = 20);

// Titres likes de l'utilisateur (scope user-library-read).
std::vector<Track> my_tracks(int limit = 50);

// Playlists de l'utilisateur (scope playlist-read-private).
std::vector<Playlist> my_playlists();

// Titres d'une playlist donnee.
std::vector<Track> playlist_tracks(const std::string& playlistId);

// URL du flux audio d'un titre (Ogg/Opus). A recuperer avec auth_header()
// et des requetes Range (lecteur, phase 4).
std::string stream_url(const std::string& trackId);

// En-tete « Authorization: Bearer <token> » courant, "" si non connecte
// (rafraichit le jeton au besoin). Expose pour le lecteur audio.
std::string auth_header();

// --- Expose pour les tests (parsing pur, sans reseau) ----------------------
std::vector<Track> parse_tracks(const std::string& jsonBody);
std::vector<Playlist> parse_playlists(const std::string& jsonBody);

} // namespace tl::sbmusic

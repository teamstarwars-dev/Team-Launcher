#pragma once

// WinHTTP — HTTPS + redirects natifs (schannel), zero disque add. requis.
// Remplace Http.Shared (cpp-httplib sans OpenSSL) pour les appels sortants.

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace tl::http {

// progress(bytesTelecharges, totalOuEcritSiInconnu=-1)
using ProgressFn = std::function<void(long long done, long long total)>;

// Reponse brute (status HTTP + corps) : requis pour OAuth/Xbox ou le status
// porte une information (404 profil absent, 401 jeton refuse...).
struct Response {
    int status = 0;
    std::string body;
};

// GET petite reponse en memoire (JSON...). nullopt si echec (timeout, HTTP != 200...).
std::optional<std::string> get_string(const std::string& url,
                                      const std::atomic<bool>* cancel = nullptr);

// GET avec headers supplementaires, status HTTP conserve. nullopt si echec reseau.
std::optional<Response> get_response(const std::string& url,
                                     const std::string& extraHeaders = {},
                                     const std::atomic<bool>* cancel = nullptr);

// POST (JSON, form-urlencoded...) : contentType vide = texte brut.
// extraHeaders format WinHTTP : "Name: value\r\nName2: value2".
std::optional<Response> post_string(const std::string& url,
                                    const std::string& body,
                                    const std::string& contentType,
                                    const std::string& extraHeaders = {},
                                    const std::atomic<bool>* cancel = nullptr);

// GET vers fichier : creation des dossiers parents, retries (3), SHA1 verifie
// par l'appelant si besoin. false = echec (fichier partiel supprime).
bool get_to_file(const std::string& url, const std::filesystem::path& dest,
                 ProgressFn progress = nullptr,
                 const std::atomic<bool>* cancel = nullptr,
                 int retries = 3);

} // namespace tl::http

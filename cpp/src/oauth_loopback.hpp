#pragma once

// Capture du code d'autorisation OAuth 2.0 par redirection en BOUCLE LOCALE
// (RFC 8252 — OAuth pour applications natives). On ouvre le navigateur système
// sur l'écran d'autorisation, et le serveur d'auth redirige vers
// http://127.0.0.1:<port>/<path>?code=…&state=… : un petit serveur éphémère
// capte cette requête, vérifie le `state` et rend le code.
//
// Implémenté dans localapi.cpp : c'est le SEUL TU qui paie l'inclusion de
// cpp-httplib (en-tête énorme) — on y mutualise le serveur plutôt que de le
// réinclure ailleurs.

#include <functional>
#include <optional>
#include <string>

namespace tl::oauth {

// Démarre un serveur loopback one-shot sur 127.0.0.1:port, attend un hit
// GET <path> portant `code` et `state` (ou `error`). Vérifie state ==
// expectedState. Rend le code d'autorisation, ou nullopt (délai dépassé,
// erreur renvoyée par le serveur, state invalide, port déjà pris). `err`, s'il
// est fourni, reçoit un message explicatif.
//
// `onReady` (optionnel) est appelé UNE fois le serveur en écoute, avant
// d'attendre : c'est le moment d'ouvrir le navigateur, pour éviter que la
// redirection arrive avant que le listener soit prêt.
std::optional<std::string> capture_code(int port, const std::string& path,
                                        const std::string& expectedState,
                                        int timeoutSec, std::string* err,
                                        const std::function<void()>& onReady = {});

} // namespace tl::oauth

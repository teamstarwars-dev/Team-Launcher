#pragma once
// Signature des plugins (et, demain, des mods de la marketplace) — V8.
//
// CHAÎNE DE CONFIANCE
//
//     clé racine  ──signe──>  certificat d'une clé intermédiaire
//     (hors ligne)            (période de validité, usages)
//                                     │
//                                     └──signe──> publication d'un plugin
//                                     └──signe──> liste de révocation
//
// La clé racine ne quitte jamais un support hors ligne ; seule sa clé
// PUBLIQUE est compilée dans le launcher. Les intermédiaires vivent sur le
// serveur de la marketplace et tournent : un intermédiaire compromis se
// révoque sans toucher à la racine ni redistribuer le launcher.
//
// FORMAT — rien n'est re-sérialisé avant vérification
//
// Un document signé est un objet { "payload": base64, "sig": base64 }. La
// signature porte sur les OCTETS du payload décodé, et c'est ce même
// payload qu'on analyse ensuite. Re-sérialiser du JSON pour le vérifier
// est un piège classique : deux sérialisations « équivalentes » ne donnent
// pas les mêmes octets, et la tentation de « normaliser » ouvre des
// failles de confusion.
//
// Algorithmes : Ed25519 (RFC 8032) et SHA-512, via Monocypher — à
// l'identique sous Windows et Linux, et interopérable avec PyNaCl côté
// serveur.

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace tl::signing {

using PublicKey = std::array<std::uint8_t, 32>;

// Clés racines épinglées. Plusieurs sont admises pour permettre une
// rotation de la racine elle-même : on publie un launcher qui connaît
// l'ancienne ET la nouvelle, puis on retire l'ancienne plus tard.
struct Roots {
    std::vector<PublicKey> keys;
};

// Les racines compilées dans ce launcher. VIDE tant que la cérémonie de
// génération de la clé racine n'a pas eu lieu : tout plugin est alors
// « non signé », faute de pouvoir vérifier quoi que ce soit.
const Roots& builtin_roots();

// --- Liste de révocation ----------------------------------------------------

struct Revocations {
    std::int64_t seq = -1;      // numéro de version, croissant
    std::int64_t issuedAt = 0;  // horodatage d'émission (s)
    bool killAll = false;       // kill-switch distant : TOUS les plugins
    std::set<std::string> keyIds;                            // intermédiaires
    std::set<std::pair<std::string, std::string>> releases;  // (id, version)
    std::set<std::string> fileHashes;                        // SHA-512 hex
};

// Vérifie et lit une liste de révocation. `minSeq` : la plus haute déjà
// acceptée — une liste PLUS ANCIENNE est refusée, sans quoi un attaquant
// rejouerait une vieille liste où son plugin n'est pas encore révoqué.
std::optional<Revocations> parse_revocations(const std::string& text, const Roots& roots,
                                             std::int64_t now, std::int64_t minSeq,
                                             std::string* err);

// Liste en vigueur dans ce processus (chargée depuis le cache disque au
// premier appel). install_revocations la remplace si elle est valide et
// plus récente, et l'écrit dans le cache.
// Rendue PAR VALEUR, copiée sous verrou : une référence pourrait être lue
// pendant qu'install_revocations la remplace depuis un autre fil.
Revocations current_revocations();
bool install_revocations(const std::string& text, std::int64_t now, std::string* err);

// --- Vérification d'un plugin ----------------------------------------------

enum class Status { Unsigned, Valid, Invalid, Revoked };

struct Verdict {
    Status status = Status::Unsigned;
    std::string detail; // pourquoi, à afficher
    std::string keyId;  // intermédiaire qui a signé
};

// Vérifie le dossier d'un plugin : signature.json présent ? chaîne jusqu'à
// une racine ? identifiant et version conformes ? et surtout : la liste
// des fichiers du dossier est-elle EXACTEMENT celle qui a été signée ?
//
// Un fichier en trop rend le plugin invalide. Sans cette règle, déposer
// une DLL à côté d'un exécutable signé suffirait à la faire charger.
// Les liens symboliques sont refusés : ils pourraient pointer hors du
// dossier.
Verdict verify_plugin_dir(const std::filesystem::path& dir, const std::string& pluginId,
                          const std::string& version, const Roots& roots,
                          const Revocations* revs, std::int64_t now);

// Même vérification pour une publication de la marketplace, plugin ou mod.
// Un mod est signé en « tl-mod-release » par une clé d'usage « mod ».
enum class Kind { Plugin, Mod };
Verdict verify_release_dir(const std::filesystem::path& dir, Kind kind, const std::string& id,
                           const std::string& version, const Roots& roots,
                           const Revocations* revs, std::int64_t now);

// Vérifie un PAQUET de mise à jour du launcher (auto-update) : signature
// Ed25519 jusqu'à une racine compilée (usage « launcher »), période de
// validité, et SHA-512 du fichier identique à celui signé. `sigText` est le
// document { "v":1, "cert":…, "release":{ "type":"tl-launcher-release",
// "version", "keyId", "signedAt", "sha512" } } publié à côté du paquet.
// Valid = installable. Tout le reste (y compris « non vérifiable faute de
// racine ») interdit l'installation.
Verdict verify_update(const std::filesystem::path& packageFile, const std::string& sigText,
                      const Roots& roots, std::int64_t now);

// --- Utilitaires exposés pour les tests et l'outillage ---------------------

std::optional<std::string> sha512_file_hex(const std::filesystem::path& p);
std::string sha512_hex(const std::string& data);

} // namespace tl::signing

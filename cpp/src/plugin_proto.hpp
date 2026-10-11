#pragma once
// Protocole IPC des plugins — V8.
//
// Trois processus, et la frontière de confiance passe entre eux :
//
//     launcher  <──(1)──>  plugin host  <──(2)──>  plugin
//
// Le plugin n'est PAS de confiance : c'est du code tiers, signé et relu,
// mais qu'on doit supposer capable de tout. Il ne parle qu'au host (2),
// jamais au launcher. Le host analyse donc les octets non fiables ; s'il
// est compromis, c'est un processus sans jeton Microsoft ni secret qui
// l'est. Il ne transmet au launcher (1) que des appels déjà validés et
// autorisés par les permissions du plugin.
//
// Ce module est la grammaire commune aux deux liens. Il ne dépend que de
// la bibliothèque standard et de nlohmann/json, pour être lié à la fois
// par tl_core et par tl_plugin_host — qui, lui, ne lie PAS tl_core.
//
// Format d'une trame : longueur sur 4 octets (petit-boutiste) puis un
// document JSON UTF-8. Une requête :
//
//     {"v":1, "id":7, "method":"instances.list", "params":{}}
//
// Une réponse porte le même id et soit "result", soit "error".
//
// Principe directeur : TOUT ce qui n'est pas explicitement attendu est
// refusé. Clé inconnue, type inattendu, chaîne trop longue, méthode hors
// table, imbrication excessive : refus, jamais tolérance. Une API qu'on
// élargit plus tard est facile ; une API trop permissive dès le départ
// ne se resserre plus sans casser les plugins.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace tl::pluginproto {

// Version de l'API exposée aux plugins. Une évolution incompatible
// incrémente ce numéro ; le host refuse un plugin qui exige une version
// qu'il ne connaît pas, plutôt que de deviner.
constexpr int kApiVersion = 1;

// Plafond d'une trame. Aucun appel de l'API v1 n'approche cette taille ;
// au-delà, c'est un plugin défectueux ou hostile, et la connexion est
// fermée — on ne tente pas de « rattraper » un flux désynchronisé.
constexpr std::size_t kMaxFrame = 64 * 1024;

// Imbrication JSON maximale. Vérifiée AVANT l'analyse : un document
// « [[[[…]]]] » de 64 Kio tiendrait dans la trame, et sa destruction
// récursive épuiserait la pile du host.
constexpr int kMaxDepth = 16;

// Codes d'erreur. Les négatifs reprennent JSON-RPC 2.0 ; les autres sont
// propres au launcher.
enum ErrorCode : int {
    kParseError = -32700,
    kInvalidRequest = -32600,
    kMethodNotFound = -32601,
    kInvalidParams = -32602,
    kInternal = -32603,
    kPermissionDenied = 1001,
    kRateLimited = 1002,
    kPluginDisabled = 1003, // kill-switch ou révocation
};

// --- Trames ---------------------------------------------------------------

std::string encode_frame(const std::string& payload);

// Décodeur incrémental : on lui donne des octets au fil de l'eau, il rend
// les trames complètes. Une trame hors limites EMPOISONNE le lecteur :
// toutes les lectures suivantes échouent, et l'appelant doit fermer la
// connexion. Après une longueur mensongère, on ne sait plus où commence
// la trame suivante — continuer, ce serait interpréter n'importe quoi.
class FrameReader {
public:
    enum class Status { Frame, NeedMore, Invalid };
    void feed(const char* data, std::size_t n);
    Status next(std::string* out);
    bool poisoned() const { return poisoned_; }

private:
    std::string buf_;
    bool poisoned_ = false;
};

// --- Permissions ----------------------------------------------------------

// Les permissions qu'un plugin peut déclarer dans plugin.json. Toute autre
// chaîne rend le manifeste invalide : une permission inconnue est soit
// une faute de frappe, soit une tentative de deviner une API cachée.
const std::vector<std::string>& known_permissions();
bool is_known_permission(const std::string& p);

// Libellé lisible, montré à l'utilisateur au moment de l'autorisation.
std::string permission_label(const std::string& p);

// --- Requêtes -------------------------------------------------------------

struct Request {
    std::int64_t id = 0;
    std::string method;
    nlohmann::json params; // toujours un objet, déjà validé
};

struct Error {
    int code = kInternal;
    std::string message;
    std::int64_t id = -1; // -1 : id illisible, réponse sans id
};

// Analyse ET valide une requête de plugin. Rend nullopt + `err` au premier
// écart. La validation des paramètres est propre à chaque méthode : à la
// sortie, `params` a exactement la forme attendue, rien de plus.
std::optional<Request> parse_request(const std::string& frame, Error* err);

// Permission requise par une méthode ; "" pour une méthode toujours
// permise (host.hello, log.write). nullopt si la méthode n'existe pas.
std::optional<std::string> required_permission(const std::string& method);

// --- Réponses -------------------------------------------------------------

std::string make_result(std::int64_t id, const nlohmann::json& result);
std::string make_error(const Error& e);

// --- Utilitaires de validation, exposés pour les tests ---------------------

// Profondeur d'imbrication de `[` et `{` hors chaînes. Rend kMaxDepth + 1
// dès que la limite est franchie, sans parcourir la suite.
int json_depth(const std::string& s);

// Identifiant d'instance du launcher : 32 caractères hexadécimaux.
bool is_instance_id(const std::string& s);

} // namespace tl::pluginproto

#include "social.hpp"

#include "datastore.hpp"

#include <mutex>

namespace tl::social {

namespace {

// Le SDK social de Discord est une bibliotheque native absente du depot :
// elle se recupere sur le portail developpeur, apres acceptation des
// conditions, et se lie a l'application. Tant qu'elle n'est pas la, ce
// drapeau reste faux et TOUTES les operations echouent proprement.
//
// On ne simule rien. Une liste d'amis fictive ou une conversation de
// demonstration donneraient l'illusion d'une fonctionnalite qui n'existe
// pas : l'utilisateur croirait ses messages envoyes.
#ifdef TL_HAS_DISCORD_SOCIAL
constexpr bool kSdkPresent = true;
#else
constexpr bool kSdkPresent = false;
#endif

std::mutex& lock() {
    static std::mutex m;
    return m;
}

bool g_started = false;

const char* not_ready_reason() {
    if (!kSdkPresent)
        return "Le SDK social de Discord n'est pas intégré à cette version. "
               "Il se récupère sur le portail développeur Discord, puis se "
               "lie au launcher : les amis et les messages privés ne passent "
               "pas par le canal local utilisé pour la Rich Presence, qui ne "
               "sait que déclarer une activité.";
    if (!DataStore::settings.discordEnabled)
        return "L'intégration Discord est désactivée dans les paramètres.";
    return "Discord n'est pas joignable. Vérifie qu'il est lancé et connecté.";
}

} // namespace

Status status() {
    std::lock_guard<std::mutex> lk(lock());
    Status s;
    s.providerName = "Discord";
    // `ready` exige a la fois le SDK, l'integration activee et la liaison
    // etablie. Les trois conditions sont distinctes, et le message le dit.
    s.ready = kSdkPresent && DataStore::settings.discordEnabled && g_started;
    // Le vocal viendra dans une phase separee, une fois le texte valide en
    // usage reel. Le bouton existe deja cote interface, desactive.
    s.voiceAvailable = false;
    if (!s.ready) s.detail = not_ready_reason();
    return s;
}

std::vector<Friend> friends() {
    if (!status().ready) return {};
    // Implementation a venir avec le SDK.
    return {};
}

std::vector<Message> conversation(const std::string&) {
    if (!status().ready) return {};
    return {};
}

namespace {

bool refuse(std::string* errOut) {
    if (errOut) *errOut = not_ready_reason();
    return false;
}

} // namespace

bool add_friend(const std::string& pseudo, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (pseudo.empty()) {
        if (errOut) *errOut = "Indique un pseudo.";
        return false;
    }
    return refuse(errOut);
}

bool send(const std::string& friendId, const std::string& text,
          std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (friendId.empty() || text.empty()) {
        if (errOut) *errOut = "Message vide.";
        return false;
    }
    return refuse(errOut);
}

bool set_blocked(const std::string& friendId, bool, std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (friendId.empty()) return refuse(errOut);
    return refuse(errOut);
}

bool report(const std::string& friendId, const std::string& reason,
            std::string* errOut) {
    if (!status().ready) return refuse(errOut);
    if (friendId.empty() || reason.empty()) {
        if (errOut) *errOut = "Indique un motif.";
        return false;
    }
    return refuse(errOut);
}

void start() {
    std::lock_guard<std::mutex> lk(lock());
    g_started = kSdkPresent;
}

void stop() {
    std::lock_guard<std::mutex> lk(lock());
    g_started = false;
}

} // namespace tl::social

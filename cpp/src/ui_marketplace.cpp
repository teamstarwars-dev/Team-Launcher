#include "ui_internal.hpp"

#include "marketplace.hpp"
#include "plugin_proto.hpp"
#include "plugin_telemetry.hpp"
#include "plugins.hpp"

#include <ctime>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// V8 — marketplace : catalogue, installation vérifiée, liste de révocation.
//
// Le réseau tourne dans un fil à part ; l'interface ne lit que l'état
// partagé, sous verrou. Une seule opération à la fois : installer pendant
// qu'on rafraîchit la liste de révocation, c'est vérifier avec une liste
// qui change sous nos pieds.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

constexpr std::int64_t kRevocationEvery = 6 * 3600;
constexpr std::int64_t kTelemetryEvery = 3600;

struct MarketState {
    std::mutex m;
    // écrits par le fil (sous m)
    bool busy = false;
    std::string busyLabel;
    bool catalogLoaded = false;
    std::vector<market::Entry> catalog;
    std::string message;
    bool messageOk = true;
    bool revSynced = false; // à reporter dans la configuration (fil principal)
    std::string revError;   // dernier échec de synchronisation (vide = aucun)

    // fil principal uniquement
    std::thread th;
    std::atomic<bool> cancel{false};
    bool revTriedThisSession = false;
    std::int64_t revNextTry = 0; // après un échec : pas avant (en mémoire seulement)
    bool catalogAutoTried = false; // chargé tout seul à la première ouverture
    std::int64_t telemetryNext = 0;  // prochain envoi possible (en mémoire)
    char urlBuf[300] = {};
    bool urlInit = false;
    int tab = 0; // 0 = plugins, 1 = mods
};
MarketState s_mk;

// Lance `job` dans le fil, si aucun autre ne tourne.
template <class F> bool run_job(const char* label, F job) {
    {
        std::lock_guard<std::mutex> lk(s_mk.m);
        if (s_mk.busy) return false;
        s_mk.busy = true;
        s_mk.busyLabel = label;
    }
    if (s_mk.th.joinable()) s_mk.th.join(); // fil précédent, déjà terminé
    s_mk.cancel = false;
    s_mk.th = std::thread([job] {
        job();
        std::lock_guard<std::mutex> lk(s_mk.m);
        s_mk.busy = false;
    });
    return true;
}

void set_message(const std::string& m, bool ok) {
    std::lock_guard<std::mutex> lk(s_mk.m);
    s_mk.message = m;
    s_mk.messageOk = ok;
}

void start_revocation_sync(bool manual) {
    run_job("Synchronisation de la liste de révocation…", [manual] {
        std::string err;
        const bool ok = market::refresh_revocations(&err, &s_mk.cancel);
        std::lock_guard<std::mutex> lk(s_mk.m);
        if (ok) s_mk.revSynced = true;
        // Erreur sur sa propre ligne : un message du catalogue ne doit pas
        // la faire disparaître, c'est une information de sécurité.
        s_mk.revError = ok ? std::string() : err;
        if (manual && ok) {
            s_mk.message = "Liste de révocation à jour.";
            s_mk.messageOk = true;
        }
    });
}

void start_catalog() {
    run_job("Chargement du catalogue…", [] {
        std::string err;
        auto c = market::fetch_catalog(&err, &s_mk.cancel);
        std::lock_guard<std::mutex> lk(s_mk.m);
        if (c) {
            s_mk.catalog = std::move(*c);
            s_mk.catalogLoaded = true;
            s_mk.message.clear();
        } else {
            s_mk.message = "Catalogue : " + err + ".";
            s_mk.messageOk = false;
        }
    });
}

void start_install(const market::Entry& e, const std::filesystem::path& modsDir) {
    run_job("Installation…", [e, modsDir] {
        // Liste de révocation fraîche AVANT d'installer : on ne veut pas
        // installer une version révoquée il y a une heure.
        std::string err;
        const bool revOk = market::refresh_revocations(&err, &s_mk.cancel);
        {
            std::lock_guard<std::mutex> lk(s_mk.m);
            if (revOk) s_mk.revSynced = true;
            s_mk.revError = revOk ? std::string() : err;
        }
        const auto r = market::install(e, modsDir, &s_mk.cancel);
        set_message(r.message, r.ok);
    });
}

std::string date_of(std::int64_t t) {
    if (t <= 0) return "jamais";
    char buf[32];
    const std::time_t tt = static_cast<std::time_t>(t);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    std::strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M", &tm);
    return buf;
}

} // namespace

void marketplace_telemetry_pump(std::int64_t now);

// Appelé à chaque image : synchronisation périodique de la liste de
// révocation, et report des résultats du fil dans la configuration.
void marketplace_pump() {
    bool synced = false;
    {
        std::lock_guard<std::mutex> lk(s_mk.m);
        synced = s_mk.revSynced;
        s_mk.revSynced = false;
    }
    auto& st = DataStore::settings;
    const auto now = static_cast<std::int64_t>(std::time(nullptr));
    if (synced) {
        st.lastRevocationSyncUnix = now;
        DataStore::save();
    }
    if (st.marketplaceUrl.empty()) return;
    marketplace_telemetry_pump(now);
    // Au démarrage (une fois), puis toutes les 6 h.
    const bool due = !s_mk.revTriedThisSession ||
                     (now - st.lastRevocationSyncUnix > kRevocationEvery && now >= s_mk.revNextTry);
    if (due) {
        bool busy;
        {
            std::lock_guard<std::mutex> lk(s_mk.m);
            busy = s_mk.busy;
        }
        if (!busy) {
            s_mk.revTriedThisSession = true;
            // Si elle échoue, on retente dans 10 min, pas à chaque image.
            s_mk.revNextTry = now + 600;
            start_revocation_sync(false);
        }
    }
}

void marketplace_telemetry_pump(std::int64_t now) {
    if (!DataStore::settings.pluginTelemetry || DataStore::settings.marketplaceUrl.empty()) return;
    if (now < s_mk.telemetryNext || plugtele::pending() == 0) return;
    {
        std::lock_guard<std::mutex> lk(s_mk.m);
        if (s_mk.busy) return;
    }
    s_mk.telemetryNext = now + kTelemetryEvery;
    run_job("Envoi des statistiques anonymes…", [] {
        std::string err;
        market::send_telemetry(&err, &s_mk.cancel); // échec : remis en attente, sans bruit
    });
}

void marketplace_stop() {
    s_mk.cancel = true;
    if (s_mk.th.joinable()) s_mk.th.join();
}

void marketplace_panel() {
    auto& st = DataStore::settings;
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr("MARKETPLACE"));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Plugins et mods relus par l'équipe, en complément de CurseForge et "
           "Modrinth. Chaque archive est vérifiée par sa signature avant "
           "d'être installée : le serveur ne peut pas faire installer ce que "
           "l'équipe n'a pas signé. Un plugin installé reste à autoriser.",
           "Plugins and mods reviewed by the team, alongside CurseForge and "
           "Modrinth. Every archive is checked against its signature before "
           "it is installed: the server cannot install anything the team did "
           "not sign. An installed plugin still has to be allowed."));
    ImGui::PopStyleColor();

    // --- Adresse du serveur ----------------------------------------------
    // L'adresse du marketplace est EMBARQUEE (pre-remplie par defaut) : plus de
    // champ ici, l'utilisateur ne la configure pas.

    // Instantané de l'état partagé : on dessine SANS tenir le verrou, car
    // les boutons ci-dessous lancent des tâches qui le prennent.
    bool busy, catalogLoaded, messageOk;
    std::string busyLabel, message, revError;
    std::vector<market::Entry> catalog;
    {
        std::lock_guard<std::mutex> lk(s_mk.m);
        busy = s_mk.busy;
        busyLabel = s_mk.busyLabel;
        message = s_mk.message;
        messageOk = s_mk.messageOk;
        catalogLoaded = s_mk.catalogLoaded;
        catalog = s_mk.catalog;
        revError = s_mk.revError;
    }
    const bool configured = !st.marketplaceUrl.empty();
    const auto revs = signing::current_revocations();

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text("%s %s%s", tr("Liste de révocation :", "Revocation list:"),
                date_of(st.lastRevocationSyncUnix).c_str(),
                revs.seq > 0 ? (" (n° " + std::to_string(revs.seq) + ")").c_str() : "");
    ImGui::PopStyleColor();
    if (!revError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE05050));
        ImGui::TextWrapped("%s %s", tr("Synchronisation refusée :", "Sync refused:"),
                           revError.c_str());
        ImGui::PopStyleColor();
    }
    if (signing::builtin_roots().keys.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE0A030));
        ImGui::TextWrapped("%s", tr("Ce launcher ne contient encore aucune clé racine : aucune "
                                    "signature ne peut être vérifiée, donc rien ne s'installe "
                                    "depuis la marketplace.",
                                    "This launcher has no root key yet: no signature can be "
                                    "verified, so nothing installs from the marketplace."));
        ImGui::PopStyleColor();
    }
    if (revs.killAll) {
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE05050));
        ImGui::TextWrapped("%s", tr("Arrêt d'urgence décidé par l'équipe : tous les plugins sont "
                                    "arrêtés.",
                                    "Emergency stop issued by the team: all plugins are "
                                    "stopped."));
        ImGui::PopStyleColor();
    }

    // --- Télémétrie anonyme (désactivée par défaut) ------------------------
    bool tele = st.pluginTelemetry;
    if (ImGui::Checkbox(tr("Envoyer des statistiques anonymes sur les plugins",
                           "Send anonymous plugin statistics"),
                        &tele)) {
        st.pluginTelemetry = tele;
        if (!tele) plugtele::take(); // désactivée : ce qui attendait est jeté
        DataStore::save();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Aide l'équipe à repérer vite un plugin qui dérape, pour le révoquer. Est envoyé, "
           "au plus une fois par heure : l'identifiant et la version d'un plugin SIGNÉ, la "
           "raison d'un incident (arrêt d'office, appel refusé, démarrage raté) et leur "
           "nombre, la version du launcher et le système. Jamais : votre pseudo, vos "
           "instances, vos fichiers, ni les plugins non signés. Le serveur ne garde que des "
           "totaux par jour, sans votre adresse.",
           "Helps the team quickly spot a misbehaving plugin, to revoke it. Sent at most once "
           "an hour: the id and version of a SIGNED plugin, the reason for an incident "
           "(forced stop, denied call, failed start) and their count, the launcher version "
           "and the OS. Never: your username, your instances, your files, or unsigned "
           "plugins. The server only keeps daily totals, without your address."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    ImGui::BeginDisabled(!configured || busy);
    if (ImGui::Button(tr("Actualiser le catalogue", "Refresh catalog"), ImVec2(220, 30)))
        start_catalog();
    ImGui::SameLine();
    if (ImGui::Button(tr("Synchroniser la révocation", "Sync revocation"), ImVec2(220, 30)))
        start_revocation_sync(true);
    ImGui::EndDisabled();
    if (busy) {
        ImGui::SameLine();
        ImGui::TextUnformatted(busyLabel.c_str());
    }
    if (!message.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, messageOk ? hex(0x50B070) : hex(0xE05050));
        ImGui::TextWrapped("%s", message.c_str());
        ImGui::PopStyleColor();
    }
    if (configured && !catalogLoaded && !busy && !s_mk.catalogAutoTried) {
        s_mk.catalogAutoTried = true;
        start_catalog();
    }
    if (!configured || !catalogLoaded) return;

    ImGui::RadioButton(tr("Plugins"), &s_mk.tab, 0);
    ImGui::SameLine();
    ImGui::RadioButton(tr("Mods"), &s_mk.tab, 1);

    const nlohmann::json* inst = selected_instance();
    std::filesystem::path modsDir;
    std::string instName;
    if (inst) {
        modsDir = DataStore::instancesRoot() / inst->value("Id", "") / "mods";
        instName = inst->value("Name", "");
    }
    std::map<std::string, std::string> installed; // id -> version
    if (s_mk.tab == 0)
        for (const auto& p : plugins::list()) installed[p.id] = p.version;
    else
        ImGui::TextWrapped("%s %s", tr("Instance cible :", "Target instance:"),
                           inst ? instName.c_str() : tr("aucune (choisissez-en une)",
                                                       "none (pick one)"));

    int shown = 0;
    for (const auto& e : catalog) {
        if ((e.kind == signing::Kind::Plugin) != (s_mk.tab == 0)) continue;
        ++shown;
        ImGui::PushID((e.slug + (s_mk.tab ? "#m" : "#p")).c_str());
        ImGui::Separator();
        ImGui::TextUnformatted(e.name.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("%s · %s", e.version.c_str(), e.slug.c_str());
        ImGui::PopStyleColor();
        if (!e.description.empty()) ImGui::TextWrapped("%s", e.description.c_str());
        if (e.kind == signing::Kind::Plugin) {
            if (e.permissions.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(tr("    aucune permission", "    no permission"));
                ImGui::PopStyleColor();
            }
            for (const auto& perm : e.permissions) {
                ImGui::Bullet();
                const bool risky = perm == "game:launch";
                if (risky) ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE0A030));
                ImGui::TextUnformatted(pluginproto::permission_label(perm).c_str());
                if (risky) ImGui::PopStyleColor();
            }
        }
        const auto it = installed.find(e.slug);
        const bool upToDate = it != installed.end() && it->second == e.version;
        const char* label = e.kind == signing::Kind::Mod ? tr("Ajouter à l'instance", "Add to instance")
                            : upToDate                   ? tr("Installé", "Installed")
                            : it != installed.end()      ? tr("Mettre à jour", "Update")
                                                         : tr("Installer", "Install");
        ImGui::BeginDisabled(busy || upToDate ||
                             (e.kind == signing::Kind::Mod && modsDir.empty()));
        if (ImGui::Button(label, ImVec2(200, 28))) start_install(e, modsDir);
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    if (!shown) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Rien dans cette catégorie pour l'instant.",
                                  "Nothing in this category yet."));
        ImGui::PopStyleColor();
    }
}

} // namespace tl::ui

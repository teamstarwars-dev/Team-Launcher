#include "ui_internal.hpp"

#include "icons.hpp"
#include "maintenance.hpp"
#include "startup.hpp"
#include "support.hpp"

#include <ctime>
#include <thread>

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

// ---------------------------------------------------------------------------
// Page Aide (17) — phase 7.
//
// Tout ce qu'un utilisateur cherche quand quelque chose ne va pas, au meme
// endroit : de quoi lire la documentation, de quoi nous ecrire, et surtout
// de quoi joindre a son message UN fichier qui contient le journal, le
// rapport machine et la configuration expurgee. Sans cela, un rapport de
// bug se resume presque toujours a « ça marche pas », et la premiere
// reponse consiste a reclamer ces trois fichiers un par un.
//
// La sortie explicite vit ici aussi : quand la croix est reglee sur
// « réduire », il faut bien un endroit ou quitter pour de bon.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

// Export des journaux : zip + diagnostic reseau, donc plusieurs secondes.
// Sur le fil de l'interface, la fenetre gelerait.
struct Export {
    std::mutex m;
    std::thread th;
    bool busy = false;
    bool done = false;       // resultat pas encore consomme
    bool ok = false;
    std::string message;
    std::string file;
};
Export ex;

// Recupere le thread termine. Appele en debut de frame, hors verrou au
// moment du join : le worker prend `m` dans son bloc final.
void export_reap() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(ex.m);
        if (ex.busy || !ex.th.joinable()) return;
        th = std::move(ex.th);
    }
    th.join();
}

void export_start(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(ex.m);
        if (ex.busy) return;
        ex.busy = true;
        ex.done = false;
        ex.message.clear();
    }
    export_reap();
    const int tid = apptasks_begin("Export des journaux", "Collecte...");
    std::lock_guard<std::mutex> lk(ex.m);
    ex.th = std::thread([path, tid] {
        std::string err;
        const bool ok = support::export_logs(path, &err);
        apptasks_end(tid, ok ? "" : err);
        std::lock_guard<std::mutex> lk2(ex.m);
        ex.busy = false;
        ex.done = true;
        ex.ok = ok;
        ex.file = path;
        ex.message = ok ? path : err;
    });
}

// Lien cliquable : bouton discret + URL en dessous, pour qu'on sache ou
// l'on va avant de cliquer (et qu'on puisse la recopier).
void link_row(icons::Id ic, const char* label, const char* help,
              const std::string& url, bool enabled = true) {
    ImGui::PushID(label);
    const float h = 34.0f;
    ImGui::BeginDisabled(!enabled || url.empty());
    const ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Button("##go", ImVec2(230, h))) open_url(url);
    ImGui::EndDisabled();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 col = ImGui::ColorConvertFloat4ToU32(
        (enabled && !url.empty()) ? kText : kDim);
    icons::draw(dl, ic, ImVec2(p.x + 9, p.y + (h - 16) * 0.5f), 16.0f, col);
    dl->AddText(ImVec2(p.x + 33, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f),
                col, tr(label));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped("%s", url.empty() ? tr(help) : url.c_str());
    ImGui::PopStyleColor();
    ImGui::PopID();
}

// --- Verification automatique des mises a jour -----------------------------
// Reglage « Vérifier les mises à jour toutes les N h ». La requete part
// sur un fil de fond ; le resultat n'est PAS affiche depuis ce fil (le
// toast vit dans l'etat de l'interface, sans verrou) mais depose ici et
// consomme a la frame suivante par whatsnew_modal(), qui tourne a chaque
// frame quelle que soit la page ouverte.
struct AutoUpd {
    std::mutex m;
    std::thread th;
    bool busy = false;
    bool pending = false;   // resultat a annoncer
    std::string version;    // vide = rien de neuf
};
AutoUpd au;

void autoupdate_reap() {
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(au.m);
        if (au.busy || !au.th.joinable()) return;
        th = std::move(au.th);
    }
    th.join();
}

// --- Notes de version ------------------------------------------------------

bool wnOpen = false;       // modale demandee
bool wnRequest = false;    // ouverture a declencher a la prochaine frame
std::string wnFrom;        // version precedente ("" = premiere execution)

} // namespace

void autoupdate_init() {
    auto& s = DataStore::settings;
    if (s.updateFreqHours <= 0) return;
    const long long now =
        static_cast<long long>(std::time(nullptr));
    const long long due =
        s.lastUpdateCheckUnix + s.updateFreqHours * 3600LL;
    // Horloge reculee (fuseau, remise a l'heure) : une date de derniere
    // verification dans le futur bloquerait la verification pour toujours.
    if (s.lastUpdateCheckUnix <= now && now < due) return;
    // Marque AVANT de partir : un echec reseau ne doit pas faire retenter
    // a chaque demarrage.
    s.lastUpdateCheckUnix = now;
    DataStore::save();
    std::lock_guard<std::mutex> lk(au.m);
    if (au.busy) return;
    au.busy = true;
    au.th = std::thread([] {
        std::string err;
        auto info = updates::check(&err);
        std::lock_guard<std::mutex> lk2(au.m);
        au.busy = false;
        // Silencieux quand il n'y a rien, et silencieux en cas d'echec :
        // une verification que personne n'a demandee n'a pas a interrompre
        // qui que ce soit pour dire que le reseau est coupe.
        if (info) {
            au.pending = true;
            au.version = info->version;
        }
    });
}

void whatsnew_init() {
    auto& s = DataStore::settings;
    const std::string cur = TL_VERSION_STRING;
    // Premiere execution : on n'a rien de neuf a raconter a quelqu'un qui
    // n'a jamais utilise la version d'avant. On enregistre, sans montrer.
    if (!s.lastRunVersion.empty() && s.lastRunVersion != cur) {
        wnFrom = s.lastRunVersion;
        wnRequest = true;
    }
    if (s.lastRunVersion != cur) {
        s.lastRunVersion = cur;
        DataStore::save();
    }
}

void whatsnew_modal() {
    autoupdate_reap();
    {
        bool pending = false;
        std::string version;
        {
            std::lock_guard<std::mutex> lk(au.m);
            pending = au.pending;
            version = au.version;
            au.pending = false;
        }
        if (pending)
            notify_toast(tr("Mise à jour disponible", "Update available"),
                         "v" + version +
                             tr(" — Paramètres > Intégrations pour l'installer.",
                                " - Settings > Integrations to install it."));
    }
    if (wnRequest) {
        wnRequest = false;
        wnOpen = true;
        ImGui::OpenPopup("##whatsnew");
    }
    if (!wnOpen) return;
    ImGui::SetNextWindowSize(ImVec2(620, 460), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##whatsnew", nullptr,
                                ImGuiWindowFlags_NoSavedSettings))
        return;
    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Quoi de neuf"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (wnFrom.empty())
        ImGui::Text(tr("Version %s", "Version %s"), TL_VERSION_STRING);
    else
        ImGui::Text(tr("Mise à jour de la version %s vers la version %s.",
                       "Updated from version %s to version %s."),
                    wnFrom.c_str(), TL_VERSION_STRING);
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::BeginChild("##wnbody", ImVec2(0, -44));
    const auto entries = changelog_entries();
    if (entries.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s",
                           tr("Aucune note de version n'accompagne cette "
                              "mise à jour.",
                              "This update comes with no release notes."));
        ImGui::PopStyleColor();
    }
    for (const auto& e : entries) {
        ImGui::TextUnformatted(e.title.c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (!e.date.empty()) {
            ImGui::TextUnformatted(format_date(e.date).c_str());
            if (!e.tag.empty()) {
                ImGui::SameLine();
                ImGui::TextUnformatted(e.tag.c_str());
            }
        }
        ImGui::PopStyleColor();
        ImGui::TextWrapped("%s", e.text.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
    }
    ImGui::EndChild();

    if (accent_button(tr("Fermer"), ImVec2(140, 34))) {
        wnOpen = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Voir toutes les actualités",
                         "See all news"),
                      ImVec2(240, 34))) {
        wnOpen = false;
        g.page = 5;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void apply_startup_selection() {
    const auto& s = DataStore::settings;
    if (s.startupGame == "none") return;
    // « last » suit la derniere instance lancee ; toute autre valeur est un
    // identifiant d'instance fige. Dans les deux cas on verifie qu'elle
    // existe encore : une instance supprimee ne doit pas laisser une
    // selection fantome que la page Jouer tenterait de lancer.
    const std::string want = s.startupGame == "last" ? s.lastGameId : s.startupGame;
    if (want.empty()) return;
    if (find_instance(want)) g.selInstId = want;
}

void help_stop() {
    autoupdate_reap();
    {
        std::thread th;
        {
            std::lock_guard<std::mutex> lk(au.m);
            th = std::move(au.th);
        }
        // http::get_response s'arrete sur son propre delai (6 s) : rien a
        // annuler, on l'attend.
        if (th.joinable()) th.join();
    }
    export_reap();
    std::thread th;
    {
        std::lock_guard<std::mutex> lk(ex.m);
        th = std::move(ex.th);
    }
    // L'export n'est pas annulable (zip en cours d'ecriture) : on l'attend.
    // Il dure quelques secondes au pire, et l'interrompre laisserait une
    // archive tronquee que l'utilisateur enverrait quand meme.
    if (th.joinable()) th.join();
}

void help_page() {
    export_reap();
    auto& s = DataStore::settings;

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Aide"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::Text(tr("Team Launcher %s — canal %s — %s",
                   "Team Launcher %s - %s channel - %s"),
                TL_VERSION_STRING,
                s.updateChannel == "beta" ? tr("bêta", "beta")
                                          : tr("stable", "stable"),
                updates::deployment() == updates::Deploy::Installed
                    ? tr("installé", "installed")
                    : tr("portable", "portable"));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();

    ImGui::BeginChild("##helpbody");

    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Se documenter et nous écrire"));
    ImGui::Spacing();
    link_row(icons::Id::Help, "Centre d'aide", "", support::help_center_url());
    link_row(icons::Id::News, "Signaler un problème", "", support::ticket_url());
    link_row(icons::Id::Star, "Proposer une idée", "", support::suggestion_url());
    link_row(icons::Id::Account, "Salon d'entraide Discord",
             "Aucun salon configuré (Paramètres > Général).",
             s.helpDiscordUrl);

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Joindre les journaux à un signalement"));
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Crée une archive zip contenant le rapport machine, le "
           "diagnostic complet, la fin de launcher.log et la configuration "
           "expurgée : les clés d'API, jetons et adresses de webhook en "
           "sont retirés avant l'écriture.",
           "Creates a zip archive with the machine report, the full "
           "diagnostic, the tail of launcher.log and the redacted "
           "configuration: API keys, tokens and webhook URLs are stripped "
           "before writing."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    bool busy = false, done = false, ok = false;
    std::string message, file;
    {
        std::lock_guard<std::mutex> lk(ex.m);
        busy = ex.busy;
        done = ex.done;
        ok = ex.ok;
        message = ex.message;
        file = ex.file;
    }
    ImGui::BeginDisabled(busy);
    if (accent_button(busy ? tr("Export en cours...", "Exporting...")
                           : tr("Exporter les journaux (zip)",
                                "Export logs (zip)"),
                      ImVec2(280, 34))) {
        if (auto p = pick_zip_save("teamlauncher-journaux.zip"))
            export_start(*p);
    }
    ImGui::EndDisabled();
    if (done && ok) {
        ImGui::SameLine();
        if (ImGui::Button(tr("Ouvrir le dossier", "Open folder"),
                          ImVec2(190, 34)))
            open_in_explorer(std::filesystem::path(file).parent_path());
    }
    if (done) {
        ImGui::PushStyleColor(ImGuiCol_Text, ok ? kDim : kDanger);
        ImGui::TextWrapped(ok ? tr("Archive écrite : %s", "Archive written: %s")
                              : "%s",
                           message.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextUnformatted(tr("Diagnostic et version"));
    ImGui::Spacing();
    if (ImGui::Button(tr("Diagnostic du système"), ImVec2(230, 34))) {
        // Le diagnostic vit dans Paramètres > Avancé, ou il voisine avec le
        // nettoyage et le cache. On y renvoie plutot que d'en tenir deux :
        // deux boutons pour la meme chose finissent toujours par diverger.
        g.page = 8;
        g.settingsTab = 3;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Quoi de neuf", "What's new"), ImVec2(190, 34))) {
        wnFrom.clear();
        wnRequest = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Mises à jour", "Updates"), ImVec2(190, 34))) {
        g.page = 8;
        g.settingsTab = 2;
    }

    // Sortie explicite : n'a de sens que si la croix ne ferme pas. Sinon
    // ce bouton ferait doublon avec elle, et occuperait la page pour rien.
    if (s.closeBehavior == "minimize") {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();
        ImGui::TextUnformatted(tr("Quitter"));
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s",
                           tr("Le bouton de fermeture de la fenêtre est réglé "
                              "sur « réduire » : le launcher reste en cours "
                              "d'exécution. Ce bouton-ci le ferme réellement.",
                              "The window close button is set to \"minimise\": "
                              "the launcher keeps running. This button really "
                              "closes it."));
        ImGui::PopStyleColor();
        ImGui::Spacing();
        if (danger_button(tr("Quitter le launcher", "Quit the launcher"),
                          ImVec2(230, 34)))
            request_quit();
    }

    ImGui::EndChild();
}

} // namespace tl::ui

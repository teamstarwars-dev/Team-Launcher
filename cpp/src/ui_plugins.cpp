#include "ui_internal.hpp"

#include "apievents.hpp"
#include "apikeys.hpp"
#include "http_win.hpp" // redact_url : une URL d'abonnement porte un secret
#include "localapi.hpp"
#include "jvmwarm.hpp"
#include "plugins.hpp"

#include <ctime>

// ---------------------------------------------------------------------------
// Phase 8 — interface des plugins, de l'API locale et du préchauffage.
//
// L'écran des plugins est écrit autour d'une idée : **on n'autorise pas un
// nom, on autorise une commande**. La ligne exacte qui sera exécutée est
// donc affichée à côté de la case, pas cachée derrière un bouton
// « détails ». Un plugin qui lance `cmd /c format D:` doit se voir au
// premier coup d'œil.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

// Même rendu que les sections de la page Paramètres (libellé estompé,
// marges). Dupliqué ici volontairement : celui de ui_settings.cpp vit
// dans son espace de noms anonyme, et l'exporter pour trois appels
// mettrait un détail de présentation dans l'en-tête partagé.
void section_label(const char* label) {
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr(label));
    ImGui::PopStyleColor();
    ImGui::Spacing();
}

// Contexte de substitution pour l'instance sélectionnée (vide si aucune).
plugins::Context context_for(const nlohmann::json* inst,
                             const plugins::Plugin& p) {
    plugins::Context c;
    c.dataDir = DataStore::dir().string();
    c.pluginDir = p.dir.string();
    if (inst) {
        c.instanceId = inst->value("Id", "");
        c.instanceName = inst->value("Name", "");
        c.instanceDir =
            (DataStore::instancesRoot() / c.instanceId).string();
        c.gameVersion = inst->value("McVersion", "");
    }
    return c;
}

std::string s_pluginError;

} // namespace

void plugins_panel() {
    section_label("PLUGINS");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Un plugin est un dossier contenant « plugin.json », qui déclare "
           "des commandes. Elles tournent comme des programmes séparés, "
           "avec vos droits et sans bac à sable : lisez la commande avant "
           "d'autoriser. Modifier un plugin autorisé le remet "
           "automatiquement en attente.",
           "A plugin is a folder holding a \"plugin.json\" that declares "
           "commands. They run as separate programs, with your rights and "
           "with no sandbox: read the command before allowing it. Editing "
           "an allowed plugin puts it back on hold automatically."));
    ImGui::PopStyleColor();
    ImGui::Spacing();
    if (ImGui::Button(tr("Ouvrir le dossier des plugins",
                         "Open the plugins folder"),
                      ImVec2(280, 30)))
        open_in_explorer(plugins::dir());

    const auto list = plugins::list();
    if (list.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextWrapped("%s", tr("Aucun plugin installé.",
                                    "No plugin installed."));
        ImGui::PopStyleColor();
        return;
    }

    const nlohmann::json* inst = selected_instance();
    for (const auto& p : list) {
        ImGui::PushID(p.id.c_str());
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        ImGui::TextUnformatted(p.name.c_str());
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::Text("%s%s", p.version.empty() ? "" : ("v" + p.version).c_str(),
                    p.author.empty() ? "" : ("  —  " + p.author).c_str());
        ImGui::PopStyleColor();

        if (!p.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
            ImGui::TextWrapped("%s", p.error.c_str());
            ImGui::PopStyleColor();
            ImGui::PopID();
            continue;
        }
        if (!p.description.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", p.description.c_str());
            ImGui::PopStyleColor();
        }

        bool on = p.enabled;
        if (ImGui::Checkbox(tr("Autoriser ce plugin", "Allow this plugin"),
                            &on))
            plugins::set_enabled(p, on);

        // Les commandes, toujours visibles — autorisé ou non.
        for (const auto& a : p.actions) {
            const auto ctx = context_for(inst, p);
            ImGui::Bullet();
            ImGui::TextUnformatted(a.label.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("    %s", plugins::preview(a, ctx).c_str());
            ImGui::PopStyleColor();
            const bool needsInst = a.where == plugins::Where::Instance;
            ImGui::PushID(a.label.c_str());
            ImGui::BeginDisabled(!p.enabled || (needsInst && !inst));
            ImGui::SameLine();
            if (ImGui::SmallButton(tr("Exécuter", "Run"))) {
                std::string err;
                if (!plugins::run(p, a, ctx, &err))
                    s_pluginError = err;
                else
                    notify_toast(tr("Plugin"), a.label);
            }
            ImGui::EndDisabled();
            if (needsInst && !inst) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(tr("(instance requise)",
                                          "(needs an instance)"));
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        ImGui::PopID();
    }

    if (!s_pluginError.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", s_pluginError.c_str());
        ImGui::PopStyleColor();
    }
}

void localapi_panel() {
    auto& s = DataStore::settings;
    section_label("API HTTP LOCALE");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Permet à un autre outil de la machine (script, Stream Deck, "
           "bot) de lire l'état du launcher et de lancer une instance. "
           "Écoute uniquement sur 127.0.0.1, et chaque requête doit porter "
           "le jeton ci-dessous — sur une machine partagée, « localhost » "
           "n'est pas une frontière.",
           "Lets another tool on this machine (script, Stream Deck, bot) "
           "read the launcher state and start an instance. Listens on "
           "127.0.0.1 only, and every request must carry the token below - "
           "on a shared machine, \"localhost\" is not a boundary."));
    ImGui::PopStyleColor();

    bool on = s.localApiPort > 0;
    if (ImGui::Checkbox(tr("Activer l'API locale", "Enable the local API"),
                        &on)) {
        if (on) {
            if (s.localApiPort <= 0) s.localApiPort = 27850;
            if (s.localApiToken.empty())
                s.localApiToken = localapi::make_token();
            std::string err;
            localapi::start(s.localApiPort, s.localApiToken, &err);
        } else {
            localapi::stop();
            s.localApiPort = 0;
        }
        DataStore::save();
    }

    if (s.localApiPort > 0) {
        int port = s.localApiPort;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::InputInt(tr("Port"), &port)) {
            // Au-dessus de 1024 : les ports réservés demandent des droits
            // d'administrateur, et échoueraient silencieusement.
            s.localApiPort = port < 1024 ? 1024 : (port > 65535 ? 65535 : port);
            DataStore::save();
            std::string err;
            localapi::start(s.localApiPort, s.localApiToken, &err);
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Jeton (Authorization: Bearer ...)",
                                  "Token (Authorization: Bearer ...)"));
        ImGui::PopStyleColor();
        // Lecture seule : un jeton se copie, il ne se tape pas.
        std::string tok = s.localApiToken;
        ImGui::SetNextItemWidth(420.0f);
        ImGui::InputText("##apitok", tok.data(), tok.size() + 1,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copier", "Copy")))
            ImGui::SetClipboardText(s.localApiToken.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Régénérer", "Regenerate"))) {
            s.localApiToken = localapi::make_token();
            DataStore::save();
            std::string err;
            localapi::start(s.localApiPort, s.localApiToken, &err);
            notify_toast(tr("API locale", "Local API"),
                         tr("Nouveau jeton : les outils déjà configurés "
                            "devront être mis à jour.",
                            "New token: tools already configured will need "
                            "updating."));
        }

        const bool up = localapi::running();
        ImGui::PushStyleColor(ImGuiCol_Text, up ? hex(0x4ADE80) : kDanger);
        if (up)
            ImGui::Text(tr("À l'écoute sur http://127.0.0.1:%d",
                           "Listening on http://127.0.0.1:%d"),
                        localapi::port());
        else
            ImGui::TextWrapped("%s", localapi::last_error().c_str());
        ImGui::PopStyleColor();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(
            "GET /v1/status  GET /v1/instances  POST /v1/launch");
        ImGui::TextUnformatted(
            "POST /v1/diag/crash  POST /v1/diag/mods  GET /v1");
        ImGui::PopStyleColor();

        apikeys_panel();
    }
    // Les abonnements ne dépendent PAS du serveur local : ils sortent de
    // la machine. Ils restent donc visibles même API désactivée.
    events_panel();
}

void events_panel() {
    section_label("ÉVÉNEMENTS SORTANTS");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Le launcher appelle une adresse quand il se passe quelque "
           "chose. Un crash part avec sa cause DÉJÀ ANALYSÉE : le mod en "
           "faute est nommé dans le message. Le destinataire n'a donc "
           "rien à savoir des rapports de crash, et le joueur n'a rien à "
           "faire.",
           "The launcher calls an address when something happens. A crash "
           "leaves with its cause ALREADY ANALYSED: the mod at fault is "
           "named in the message. The receiver needs to know nothing about "
           "crash reports, and the player has nothing to do."));
    ImGui::TextWrapped(
        "%s",
        tr("HTTPS obligatoire (sauf adresse locale, pour vos essais) : le "
           "message contient le nom de l'instance et le secret voyage en "
           "en-tête.",
           "HTTPS required (except a local address, for testing): the "
           "message carries the instance name and the secret travels in a "
           "header."));
    ImGui::PopStyleColor();

    static char s_url[320] = "";
    static char s_secret[96] = "";
    static bool s_evCrash = true, s_evStart = false, s_evStop = false;
    static std::string s_err;

    ImGui::SetNextItemWidth(420.0f);
    ImGui::InputTextWithHint("##evurl", "https://exemple/hook", s_url,
                             sizeof(s_url));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputTextWithHint("##evsecret", tr("Secret (facultatif)"), s_secret,
                             sizeof(s_secret));
    ImGui::Checkbox(tr("crash"), &s_evCrash);
    ImGui::SameLine();
    ImGui::Checkbox(tr("lancement", "game start"), &s_evStart);
    ImGui::SameLine();
    ImGui::Checkbox(tr("fin de partie", "game stop"), &s_evStop);
    ImGui::SameLine();
    if (ImGui::Button(tr("Ajouter", "Add"), ImVec2(130, 28))) {
        std::vector<std::string> types;
        if (s_evCrash) types.push_back(events::kCrash);
        if (s_evStart) types.push_back(events::kGameStart);
        if (s_evStop) types.push_back(events::kGameStop);
        s_err.clear();
        if (events::add(trimmed(s_url), trimmed(s_secret), types, &s_err)) {
            s_url[0] = '\0';
            s_secret[0] = '\0';
        }
    }
    if (!s_err.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", s_err.c_str());
        ImGui::PopStyleColor();
    }

    const auto hooks = events::list();
    if (hooks.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Aucun abonnement.", "No subscription."));
        ImGui::PopStyleColor();
        return;
    }
    for (const auto& h : hooks) {
        ImGui::PushID(h.id.c_str());
        ImGui::Spacing();
        bool on = h.enabled;
        if (ImGui::Checkbox("##evon", &on)) events::set_enabled(h.id, on);
        ImGui::SameLine();
        // URL expurgée : beaucoup d'adresses de réception portent un
        // secret dans leur chemin (Discord, Slack...). L'afficher en clair
        // dans une capture d'écran de support le donnerait à tout le monde.
        ImGui::TextUnformatted(http::redact_url(h.url).c_str());
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        std::string ev;
        for (const auto& t : h.events) {
            if (!ev.empty()) ev += " ";
            ev += t;
        }
        ImGui::Text("    %s", ev.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Tester", "Test"))) {
            std::string err;
            if (events::test(h.id, &err))
                notify_toast(tr("Événements", "Events"),
                             tr("Message d'essai accepté.",
                                "Test message accepted."));
            else
                notify_toast(tr("Événements", "Events"), err);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Retirer", "Remove"))) events::remove(h.id);
        if (h.lastUnix != 0) {
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  h.lastOk ? hex(0x4ADE80) : kDanger);
            char when[32] = "?";
            const std::time_t t = static_cast<std::time_t>(h.lastUnix);
            if (std::tm* lt = std::localtime(&t))
                std::strftime(when, sizeof(when), "%d/%m %H:%M", lt);
            if (h.lastOk)
                ImGui::Text(tr("    OK à %s", "    OK at %s"), when);
            else
                ImGui::Text("    %s — %s", when, h.lastError.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }
}

void apikeys_panel() {
    section_label("CLÉS D'APPLICATION");
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Le jeton ci-dessus est le vôtre et donne tous les droits. Pour "
           "confier l'accès à une application tierce, créez-lui une clé "
           "dédiée : elle n'aura que les portées que vous cochez, et vous "
           "pourrez la révoquer sans gêner les autres.",
           "The token above is yours and grants everything. To give a "
           "third-party application access, create a dedicated key: it "
           "only gets the scopes you tick, and you can revoke it without "
           "disturbing the others."));
    ImGui::TextWrapped(
        "%s",
        tr("« diag » n'accède à RIEN de votre machine : l'analyse porte "
           "uniquement sur ce que l'appelant envoie. « lecture » expose vos "
           "instances. « contrôle » peut lancer une partie.",
           "\"diag\" accesses NOTHING on your machine: it only analyses what "
           "the caller sends. \"read\" exposes your instances. \"control\" "
           "can start the game."));
    ImGui::PopStyleColor();

    // --- Création ---
    static char s_name[64] = "";
    static char s_contact[96] = "";
    static bool s_diag = true, s_read = false, s_control = false;
    // Le secret n'est montré QU'UNE FOIS : il n'est pas conservé, seule
    // son empreinte l'est. On le garde donc affiché jusqu'à ce que
    // l'utilisateur le ferme lui-même.
    static std::string s_fresh, s_freshApp;

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##akname", tr("Nom de l'application"), s_name,
                             sizeof(s_name));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##akcontact", tr("Contact (facultatif)"),
                             s_contact, sizeof(s_contact));
    ImGui::Checkbox(tr("diag"), &s_diag);
    ImGui::SameLine();
    ImGui::Checkbox(tr("lecture", "read"), &s_read);
    ImGui::SameLine();
    ImGui::Checkbox(tr("contrôle", "control"), &s_control);
    ImGui::SameLine();
    if (ImGui::Button(tr("Créer une clé", "Create a key"), ImVec2(160, 28))) {
        std::vector<std::string> scopes;
        if (s_diag) scopes.push_back(apikeys::kScopeDiag);
        if (s_read) scopes.push_back(apikeys::kScopeRead);
        if (s_control) scopes.push_back(apikeys::kScopeControl);
        const auto c = apikeys::create(trimmed(s_name), trimmed(s_contact),
                                       scopes);
        s_fresh = c.secret;
        s_freshApp = c.key.appName;
        s_name[0] = '\0';
        s_contact[0] = '\0';
    }

    if (!s_fresh.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, hex(0xE0A030));
        ImGui::TextWrapped(
            tr("Clé de « %s » — copiez-la maintenant, elle ne sera plus "
               "jamais affichée.",
               "Key for \"%s\" - copy it now, it will never be shown again."),
            s_freshApp.c_str());
        ImGui::PopStyleColor();
        ImGui::SetNextItemWidth(520.0f);
        ImGui::InputText("##freshkey", s_fresh.data(), s_fresh.size() + 1,
                         ImGuiInputTextFlags_ReadOnly);
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("Copier", "Copy")))
            ImGui::SetClipboardText(s_fresh.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(tr("J'ai copié", "Copied"))) {
            s_fresh.clear();
            s_freshApp.clear();
        }
    }

    // --- Liste ---
    const auto keys = apikeys::list();
    if (keys.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(tr("Aucune clé distribuée.",
                                  "No key handed out."));
        ImGui::PopStyleColor();
        return;
    }
    if (ImGui::BeginTable("##aktable", 5,
                          ImGuiTableFlags_RowBg |
                              ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn(tr("Application"));
        ImGui::TableSetupColumn(tr("Portées", "Scopes"),
                                ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableSetupColumn(tr("Appels", "Calls"),
                                ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn(tr("Dernier usage", "Last used"),
                                ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 150.0f);
        ImGui::TableHeadersRow();
        for (const auto& k : keys) {
            ImGui::PushID(k.id.c_str());
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, k.revoked ? kDim : kText);
            ImGui::TextUnformatted(k.appName.c_str());
            ImGui::PopStyleColor();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(k.id.c_str());
            if (!k.contact.empty()) ImGui::TextUnformatted(k.contact.c_str());
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(1);
            std::string sc;
            for (const auto& s : k.scopes) {
                if (!sc.empty()) sc += " ";
                sc += s;
            }
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  k.revoked ? kDim : hex(0x4ADE80));
            ImGui::TextUnformatted(k.revoked ? tr("révoquée", "revoked")
                                             : sc.c_str());
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%lld", k.calls);

            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            if (k.lastUsedUnix == 0) {
                ImGui::TextUnformatted(tr("jamais", "never"));
            } else {
                char when[32] = "?";
                const std::time_t t = static_cast<std::time_t>(k.lastUsedUnix);
                if (std::tm* lt = std::localtime(&t))
                    std::strftime(when, sizeof(when), "%d/%m/%Y %H:%M", lt);
                ImGui::TextUnformatted(when);
            }
            ImGui::PopStyleColor();

            ImGui::TableSetColumnIndex(4);
            if (!k.revoked) {
                if (ImGui::SmallButton(tr("Révoquer", "Revoke")))
                    apikeys::revoke(k.id);
                ImGui::SameLine();
            }
            if (ImGui::SmallButton(tr("Retirer", "Remove"))) apikeys::remove(k.id);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void jvmwarm_panel() {
    auto& s = DataStore::settings;
    section_label("PRÉCHAUFFAGE AVANT LANCEMENT");
    if (ImGui::Checkbox(tr("Préparer le lancement pendant la sélection",
                           "Prepare the launch while browsing"),
                        &s.jvmPreload)) {
        if (!s.jvmPreload) jvmwarm::stop();
        DataStore::save();
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Repère Java et met en cache les gros fichiers du jeu pendant "
           "que vous choisissez votre instance. Ne garde aucune machine "
           "virtuelle allumée — ce n'est pas possible : une JVM déjà "
           "démarrée ne peut pas être redirigée vers Minecraft.",
           "Locates Java and warms the game's largest files while you pick "
           "an instance. It keeps no virtual machine running - that is not "
           "possible: an already-started JVM cannot be redirected to "
           "Minecraft."));
    ImGui::PopStyleColor();
    if (s.jvmPreload) {
        const auto st = jvmwarm::state();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        if (st.warming)
            ImGui::TextUnformatted(tr("Préparation en cours...",
                                      "Preparing..."));
        else if (!st.detail.empty())
            ImGui::TextWrapped("%s", st.detail.c_str());
        else
            ImGui::TextUnformatted(tr("Rien de préparé pour l'instant.",
                                      "Nothing prepared yet."));
        ImGui::PopStyleColor();
    }
}

} // namespace tl::ui

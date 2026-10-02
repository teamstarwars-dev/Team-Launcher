#include "ui_internal.hpp"

#include "datastore.hpp"
#include "moddev.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Developpement de mods (page 13) — portage de ModDevPage.cs. La logique est
// dans `moddev.hpp` ; cette page n'est que la facade.
//
// Elle affiche explicitement l'etat de la chaine d'outils (wrapper Gradle,
// gradle du PATH, JDK). Le C# se contentait de chercher `gradlew.bat` et de
// repondre « cree le projet d'abord » — alors que creer le projet ne le
// produisait pas, ce qui rendait Build et Run inutilisables.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

constexpr std::size_t kMaxConsoleLines = 4000;

struct DevState {
    int loader = 0; // index dans kLoaders
    int version = 1;
    char name[96] = "Mon Mod";
    char pkg[128] = "com.exemple.monmod";
    char dir[512] = "";
    char cmd[256] = "";
    bool overwrite = false;

    moddev::Toolchain tc;
    // Atomique parce que le fil d'installation la remet a false pour
    // demander une nouvelle detection. Lui ne peut pas toucher a `tc` :
    // l'interface la lit a chaque frame sans verrou, et c'est le fil de
    // l'interface qui en reste proprietaire.
    std::atomic<bool> tcChecked{false};
    bool autoToolchain = false; // TL_AUTO_TOOLCHAIN (test)

    std::mutex m;
    std::deque<std::string> console;
    bool running = false;
    std::string runningWhat;
    std::atomic<bool> cancel{false};
};
DevState D;

const moddev::Loader kLoaders[] = {moddev::Loader::Fabric, moddev::Loader::Forge,
                                   moddev::Loader::NeoForge,
                                   moddev::Loader::Bedrock};

moddev::Loader current_loader() {
    return kLoaders[std::clamp(D.loader, 0, 3)];
}

std::string current_version() {
    const auto& v = moddev::versions_for(current_loader());
    if (v.empty()) return {};
    return v[static_cast<std::size_t>(
        std::clamp(D.version, 0, static_cast<int>(v.size()) - 1))];
}

void log_line(const std::string& s) {
    std::lock_guard<std::mutex> lk(D.m);
    D.console.push_back(s);
    while (D.console.size() > kMaxConsoleLines) D.console.pop_front();
}

void recheck_toolchain() {
    // La version de Minecraft compte : elle decide du JDK necessaire.
    // Sans elle, on annoncait « JDK oui » a quelqu'un qui n'a qu'un
    // JDK 8 pour un mod 1.21.
    D.tc = moddev::detect_toolchain(std::filesystem::path(D.dir),
                                    current_version());
    D.tcChecked = true;
}

// Lance `work` sur un thread de fond, avec l'entree dans le panneau des taches.
void spawn(const std::string& what,
           std::function<void(const std::atomic<bool>&)> work) {
    {
        std::lock_guard<std::mutex> lk(D.m);
        if (D.running) return;
        D.running = true;
        D.runningWhat = what;
    }
    D.cancel = false;
    const int tid = apptasks_begin(what, what, &D.cancel);
    std::thread([what, work, tid] {
        work(D.cancel);
        apptasks_end(tid);
        std::lock_guard<std::mutex> lk(D.m);
        D.running = false;
        D.runningWhat.clear();
    }).detach();
}

// Couleur d'une ligne de console (meme heuristique que le C#, plus le prefixe
// « $ » des commandes saisies).
ImVec4 line_color(const std::string& s) {
    if (s.rfind("$ ", 0) == 0) return kAccent;
    if (s.find("ERROR") != std::string::npos ||
        s.find("FAILURE") != std::string::npos ||
        s.find("error:") != std::string::npos)
        return kDanger;
    if (s.find("WARN") != std::string::npos) return ImVec4(1.0f, 0.78f, 0.24f, 1.0f);
    if (s.find("BUILD SUCCESSFUL") != std::string::npos)
        return ImVec4(0.31f, 0.78f, 0.31f, 1.0f);
    return ImVec4(0.78f, 0.78f, 0.78f, 1.0f);
}

void run_gradle(const std::string& task) {
    recheck_toolchain();
    if (D.tc.command.empty()) {
        log_line(D.tc.problem);
        return;
    }
    const std::string cmdline = D.tc.command + " " + task;
    const std::filesystem::path dir(D.dir);
    spawn(task, [cmdline, dir, task](const std::atomic<bool>& cancel) {
        log_line("$ " + cmdline);
        const int code = moddev::run(dir, cmdline, log_line, &cancel);
        log_line(code == 0 ? "Termine (code 0)."
                           : "Echec : code de sortie " + std::to_string(code) +
                                 ".");
    });
}

// Telecharge ce qui manque — Gradle, et un JDK a la bonne version. Le
// launcher sait deja recuperer un JRE pour jouer ; il n'y avait aucune
// raison de renvoyer l'utilisateur installer Gradle a la main, surtout
// avec une commande qui exigeait Gradle pour s'executer.
void install_toolchain() {
    const std::string mc = current_version();
    const int needed = moddev::jdk_major_for(mc);
    const bool wantJdk = !D.tc.jdk ||
                         (D.tc.javaMajor > 0 && D.tc.javaMajor < needed);
    const bool wantGradle = D.tc.command.empty();

    spawn("Installation de la chaîne d'outils",
          [needed, wantJdk, wantGradle](const std::atomic<bool>& cancel) {
              if (wantJdk) {
                  log_line("Téléchargement du JDK " + std::to_string(needed) +
                           " (Adoptium)...");
                  // download_java prend une reference non const : on lui
                  // passe un relais branche sur l'annulation de la page.
                  static std::atomic<bool> relay{false};
                  relay.store(cancel.load());
                  const auto jdk = download_java(
                      needed, /*wantJdk=*/true,
                      [](const char* s) { if (s) log_line(s); }, relay);
                  log_line(jdk ? "JDK installé : " + *jdk
                               : "Échec du téléchargement du JDK.");
              }
              if (cancel.load()) {
                  log_line("Annulé.");
                  return;
              }
              if (wantGradle) {
                  std::string err;
                  const std::string g =
                      moddev::ensure_gradle(log_line, &cancel, &err);
                  if (g.empty()) log_line("Échec : " + err);
              }
              // Redetecter, sinon la ligne d'etat continue de reclamer ce
              // qu'on vient d'installer — defaut vu a l'ecran : le JDK et
              // Gradle etaient bien la, et la page affichait toujours
              // « Gradle : absent · JDK 8 ». Le message disait « verifie a
              // nouveau ci-dessus », ce que personne ne lisait comme un
              // ordre a executer soi-meme.
              D.tcChecked.store(false);
              log_line("Chaîne d'outils : nouvelle vérification en cours.");
          });
}

} // namespace

void moddev_page() {
    // TL_AUTO_DEV=<dossier> : prerempli le dossier de projet (test).
    static bool autoDone = false;
    if (!autoDone) {
        autoDone = true;
        if (const char* d = std::getenv("TL_AUTO_DEV"))
            std::snprintf(D.dir, sizeof(D.dir), "%s", d);
        // TL_AUTO_TOOLCHAIN=1 : déclenche l'installation sans clic. Un
        // bouton qui télécharge 300 Mo ne se vérifie pas autrement qu'en
        // le déclenchant pour de vrai.
        if (std::getenv("TL_AUTO_TOOLCHAIN")) D.autoToolchain = true;
        if (D.dir[0] == '\0') {
            const auto p = DataStore::dir() / "mods-dev" / "monmod";
            std::snprintf(D.dir, sizeof(D.dir), "%s", p.string().c_str());
        }
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Développement de mods"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s", tr("Crée un squelette de mod, puis construis-le et lance-le avec "
                 "Gradle. Un JDK 21 et Gradle (ou le wrapper du projet) sont "
                 "nécessaires pour construire.",
                 "Create a mod skeleton, then build and run it with Gradle. A "
                 "JDK 21 and Gradle (or the project wrapper) are needed to "
                 "build."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    bool running;
    std::string runningWhat;
    {
        std::lock_guard<std::mutex> lk(D.m);
        running = D.running;
        runningWhat = D.runningWhat;
    }

    // --- nouveau projet ---
    ImGui::SetNextItemWidth(150.0f);
    const char* loaderLabels[] = {"Fabric", "Forge", "NeoForge", "Bedrock"};
    if (ImGui::Combo("##devloader", &D.loader, loaderLabels, 4)) D.version = 0;
    ImGui::SameLine();
    {
        const auto& vs = moddev::versions_for(current_loader());
        D.version = std::clamp(D.version, 0, static_cast<int>(vs.size()) - 1);
        ImGui::SetNextItemWidth(120.0f);
        if (ImGui::BeginCombo("##devver", current_version().c_str())) {
            for (int i = 0; i < static_cast<int>(vs.size()); ++i)
                if (ImGui::Selectable(vs[i].c_str(), i == D.version))
                    D.version = i;
            ImGui::EndCombo();
        }
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextUnformatted(tr("chargeur et version", "loader and version"));
    ImGui::PopStyleColor();

    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::TextUnformatted(tr("Nom du mod", "Mod name"));
    ImGui::SameLine(140.0f);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("##devname", D.name, sizeof(D.name));
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    const std::string modId = moddev::mod_id_from(D.name);
    ImGui::Text("id : %s", modId.empty() ? "-" : modId.c_str());
    ImGui::PopStyleColor();

    const bool bedrock = current_loader() == moddev::Loader::Bedrock;
    ImGui::BeginDisabled(bedrock);
    ImGui::TextUnformatted(tr("Paquet Java", "Java package"));
    ImGui::SameLine(140.0f);
    ImGui::SetNextItemWidth(260.0f);
    ImGui::InputText("##devpkg", D.pkg, sizeof(D.pkg));
    ImGui::EndDisabled();
    if (!bedrock) {
        ImGui::SameLine();
        const bool okPkg = moddev::valid_package(D.pkg);
        ImGui::PushStyleColor(ImGuiCol_Text, okPkg ? kDim : kDanger);
        ImGui::TextUnformatted(
            okPkg ? (std::string("classe : ") + moddev::class_name_from(D.name))
                        .c_str()
                  : tr("paquet invalide", "invalid package"));
        ImGui::PopStyleColor();
    }

    ImGui::TextUnformatted(tr("Dossier", "Folder"));
    ImGui::SameLine(140.0f);
    ImGui::SetNextItemWidth(420.0f);
    if (ImGui::InputText("##devdir", D.dir, sizeof(D.dir))) D.tcChecked = false;
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button("...##devbrowse")) {
#ifdef _WIN32
        if (auto p = pick_folder(L"Dossier du projet")) {
#else
        if (auto p = pick_folder("Dossier du projet")) {
#endif
            std::snprintf(D.dir, sizeof(D.dir), "%s", p->c_str());
            D.tcChecked = false;
        }
    }

    ImGui::Checkbox(tr("Écraser les fichiers existants", "Overwrite existing files"),
                    &D.overwrite);

    ImGui::Spacing();
    ImGui::BeginDisabled(running);
    if (accent_button(tr("Créer le projet", "Create project"), ImVec2(180, 34))) {
        moddev::ProjectSpec spec;
        spec.loader = current_loader();
        spec.mcVersion = current_version();
        spec.name = D.name;
        spec.pkg = D.pkg;
        spec.dir = D.dir;
        const bool ow = D.overwrite;
        spawn(tr("Création du projet", "Create project"),
              [spec, ow](const std::atomic<bool>&) {
                  log_line("=== " + std::string(moddev::loader_name(spec.loader)) +
                           " " + spec.mcVersion + " ===");
                  // Resolution en ligne des versions de dependances : le C#
                  // les devinait, et le premier build echouait.
                  const auto deps = moddev::resolve_deps(spec.loader, spec.mcVersion);
                  const auto r = moddev::create_project(spec, deps, ow, log_line);
                  if (!r.ok) {
                      log_line("Echec : " + r.error);
                      return;
                  }
                  for (const auto& f : r.written) log_line("  + " + f);
                  for (const auto& f : r.kept) log_line("  = " + f + " (conserve)");
              });
        D.tcChecked = false;
    }
    ImGui::EndDisabled();

    // --- chaine d'outils ---
    if (!D.tcChecked) recheck_toolchain();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, D.tc.command.empty() || !D.tc.jdk
                                             ? kDanger
                                             : kDim);
    {
        // Gradle peut venir de trois endroits : le wrapper du projet, le
        // PATH, ou l'installation gérée par le launcher. Les confondre
        // sous « gradle : non » laissait croire qu'il manquait alors
        // qu'il était là.
        const char* gradleFrom =
            D.tc.wrapper          ? tr("wrapper du projet", "project wrapper")
            : D.tc.gradleOnPath   ? tr("PATH", "PATH")
            : D.tc.gradleManaged  ? tr("installé par le launcher",
                                       "installed by the launcher")
                                  : tr("absent", "missing");
        std::string java;
        if (!D.tc.jdk)
            java = D.tc.javaPresent ? tr("JRE seulement", "JRE only")
                                    : tr("absent", "missing");
        else
            java = "JDK " + (D.tc.javaMajor > 0
                                 ? std::to_string(D.tc.javaMajor)
                                 : std::string("?"));
        // « il en faut 21 » n'a de sens que si ce n'est pas ce qu'on a.
        // Affiche en permanence, le rappel donnait « JDK 21 (il en faut
        // 21) » : du bruit exactement au moment ou tout va bien.
        const bool javaOk = D.tc.jdk && D.tc.javaMajor >= D.tc.javaNeeded;
        if (javaOk)
            ImGui::Text(tr("Chaîne d'outils — Gradle : %s · Java : %s",
                           "Toolchain - Gradle: %s · Java: %s"),
                        gradleFrom, java.c_str());
        else
            ImGui::Text(tr("Chaîne d'outils — Gradle : %s · Java : %s (il en faut %d)",
                           "Toolchain - Gradle: %s · Java: %s (needs %d)"),
                        gradleFrom, java.c_str(), D.tc.javaNeeded);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(tr("Revérifier", "Re-check"))) recheck_toolchain();
    if (!D.tc.problem.empty()) ImGui::TextWrapped("%s", D.tc.problem.c_str());
    ImGui::PopStyleColor();
    // Le bouton n'apparaît que s'il a quelque chose à faire : proposer
    // d'installer ce qui est déjà là ferait douter de ce qu'on affiche.
    if (!D.tc.problem.empty()) {
        // Consommé une seule fois, et seulement quand rien ne tourne.
        bool autoFire = false;
        if (D.autoToolchain && !running) {
            D.autoToolchain = false;
            autoFire = true;
        }
        ImGui::BeginDisabled(running);
        if (accent_button(tr("Installer la chaîne d'outils",
                             "Install the toolchain"),
                          ImVec2(260, 30)) ||
            autoFire)
            install_toolchain();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(
            tr("Gradle (~130 Mo) et le JDK sont téléchargés dans le dossier "
               "du launcher, sans rien installer sur le système.",
               "Gradle (~130 MB) and the JDK are downloaded into the "
               "launcher's folder, nothing is installed system-wide."));
        ImGui::PopStyleColor();
    }

    // --- build / run ---
    ImGui::Spacing();
    const bool canRun = !running && !bedrock && !D.tc.command.empty();
    ImGui::BeginDisabled(!canRun);
    if (ImGui::Button(tr("Build", "Build"), ImVec2(110, 32))) run_gradle("build");
    ImGui::SameLine();
    if (ImGui::Button(tr("Run client", "Run client"), ImVec2(130, 32)))
        run_gradle("runClient");
    ImGui::SameLine();
    if (ImGui::Button(tr("Run server", "Run server"), ImVec2(130, 32)))
        run_gradle("runServer");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!running);
    if (danger_button(tr("Arrêter", "Stop"), ImVec2(110, 32))) D.cancel = true;
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(tr("Ouvrir le dossier", "Open folder"), ImVec2(180, 32)))
        open_in_explorer(std::filesystem::path(D.dir));
    if (bedrock) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDim);
        ImGui::TextUnformatted(
            tr("Un add-on Bedrock ne se construit pas avec Gradle : copie BP/ "
               "et RP/ dans les dossiers de développement de Minecraft.",
               "A Bedrock add-on is not built with Gradle: copy BP/ and RP/ "
               "into Minecraft's development folders."));
        ImGui::PopStyleColor();
    }

    if (running) {
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::Text("%s...", runningWhat.c_str());
        ImGui::PopStyleColor();
    }

    // --- console ---
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBg);
    const float consoleH = (std::max)(ImGui::GetContentRegionAvail().y - 70.0f,
                                      100.0f);
    ImGui::BeginChild("##devconsole", ImVec2(0, consoleH), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    {
        std::lock_guard<std::mutex> lk(D.m);
        ImGuiListClipper clip;
        clip.Begin(static_cast<int>(D.console.size()));
        while (clip.Step())
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; ++i) {
                const std::string& s = D.console[static_cast<std::size_t>(i)];
                ImGui::PushStyleColor(ImGuiCol_Text, line_color(s));
                ImGui::TextUnformatted(s.c_str());
                ImGui::PopStyleColor();
            }
        clip.End();
    }
    if (running && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::PopStyleColor();

    // --- commande libre ---
    ImGui::PushStyleColor(ImGuiCol_FrameBg, kBg);
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 200.0f);
    const bool enter = ImGui::InputTextWithHint(
        "##devcmd", tr("Commande à exécuter dans le dossier du projet...",
                       "Command to run in the project folder..."),
        D.cmd, sizeof(D.cmd), ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::BeginDisabled(running || D.cmd[0] == '\0');
    const bool go = ImGui::Button(tr("Exécuter", "Run"), ImVec2(110, 0));
    ImGui::EndDisabled();
    if ((enter || go) && !running && D.cmd[0] != '\0') {
        const std::string c = D.cmd;
        const std::filesystem::path dir(D.dir);
        spawn(tr("Commande", "Command"), [c, dir](const std::atomic<bool>& cancel) {
            log_line("$ " + c);
            const int code = moddev::run(dir, c, log_line, &cancel);
            if (code != 0) log_line("Code de sortie : " + std::to_string(code));
        });
        D.cmd[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Effacer", "Clear"), ImVec2(80, 0))) {
        std::lock_guard<std::mutex> lk(D.m);
        D.console.clear();
    }
}

} // namespace tl::ui

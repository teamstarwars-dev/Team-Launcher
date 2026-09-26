#include "launch_flow.hpp"

#include "backup.hpp"
#include "datastore.hpp"
#include "game_installer.hpp"
#include "http_win.hpp"
#include "ms_auth.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl {

namespace {

std::string normalize_version(const std::string& v) {
    if (v.empty() || v == "latest" || v == "?") return {};
    return v;
}

std::vector<std::string> json_string_array(const json& j) {
    std::vector<std::string> out;
    if (j.is_array())
        for (const auto& e : j)
            if (e.is_string()) out.push_back(e.get<std::string>());
    return out;
}

} // namespace

LaunchResult launch_flow(const LaunchRequest& req, const LaunchUi& ui,
                         std::atomic<bool>& cancel) {
    LaunchResult res;
    const auto t0 = std::chrono::steady_clock::now();

    auto log = [&](const std::string& s) {
        log_line(s);
        if (ui.log) ui.log(s.c_str());
    };
    auto status = [&](const char* s) {
        if (ui.status) ui.status(s);
    };
    auto elapsed = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - t0)
            .count();
    };

    try {
        status("Préparation...");
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }

        // ---- Sauvegarde auto des mondes AVANT la session (anti-corruption) ----
        // Fidele au C# : non bloquant, une ligne de journal en cas d'echec.
        if (!req.instanceId.empty()) {
            try {
                const std::string pre = backup::create(req.instanceId);
                if (!pre.empty()) log("Sauvegarde pré-session : " + pre);
            } catch (const std::exception& ex) {
                log(std::string("Échec sauvegarde pré-session : ") + ex.what());
            }
        }

        // ---- Version (derniere release si vide/latest) ----
        std::string version = normalize_version(req.version);
        if (version.empty()) {
            version = latest_release().value_or("");
            if (version.empty())
                throw std::runtime_error(
                    "Impossible de récupérer la dernière version de Minecraft.");
        }

        // ---- Session : Microsoft (module 4) ou hors ligne ----
        McSession session;
        if (DataStore::settings.accountMode == "microsoft") {
            status("Connexion à ton compte Microsoft...");
            std::string authErr;
            if (!auth::login_blocking(&cancel, &authErr)) {
                if (cancel.load()) {
                    res.cancelled = true;
                    return res;
                }
                // Divergence assumee : le C# passait une session nulle a
                // BuildGameArgs (NullReferenceException). On echoue proprement.
                throw std::runtime_error("Connexion Microsoft impossible : " + authErr);
            }
            const auto ms = auth::get_session();
            session = McSession{ms->name, ms->uuid, ms->token};
            if (DataStore::settings.playerName != session.name) {
                DataStore::settings.playerName = session.name;
                DataStore::save();
            }
            log("Compte Microsoft : connecté en tant que " + session.name + ".");
        } else {
            session = offline_session(DataStore::settings.playerName);
        }
        log("Instance « " + session.name + " » → lancement de Minecraft " +
            req.loader + " " + version);

        // ---- Fichiers du jeu ----
        json info = install(version, req.loader, ui.progress, cancel,
                            /*forceVerify=*/false);
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }
        log("Fichiers prets (" + std::to_string(elapsed()) + " ms).");

        // C# : isForge = presence de la cle (TryGetProperty(..., out _)),
        // NeoForge/Fabric ont la cle a false mais recoivent les args Forge.
        const bool isForge = info.contains("isForge");
        const int requiredJava = info.value("javaMajor", 8);

        // ---- Java ----
        log("Recherche d'un Java " + std::to_string(requiredJava) + "+...");
        std::optional<std::string> java = find_java(requiredJava);
        if (!java) {
            status(("Téléchargement de Java " +
                    std::to_string(requiredJava) + "...").c_str());
            java = download_java(requiredJava,
                                 [&status](const char* s) { status(s); }, cancel);
        }
        if (!java) {
            throw std::runtime_error(
                "Aucun Java " + std::to_string(requiredJava) +
                "+ trouvé. Installe-le depuis adoptium.net.");
        }
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }
        log("Java sélectionné : " + *java);

        // ---- RAM (meme logique de marge que le C#) ----
        const int ramWanted = std::clamp(
            req.ramGb > 0 ? req.ramGb : DataStore::settings.maxRamGb, 1, 32);
        const long long availMb = available_ram_mb();
        const long long totalMb = total_ram_mb();
        const long long headroomMb = availMb - static_cast<long long>(ramWanted) * 1024;
        log("Mémoire système : " + std::to_string(availMb) + " Mo disponibles / " +
            std::to_string(totalMb) + " Mo totales, " +
            std::to_string(ramWanted) + " Go demandés (marge: " +
            std::to_string(headroomMb) + " Mo).");
        if (headroomMb < 500 && headroomMb >= 0) {
            status(("Mémoire serrée (" + std::to_string(availMb) +
                    "Mo dispo) — ferme des programmes en arrière-plan.")
                       .c_str());
            log("Mémoire très serrée ! Seulement " + std::to_string(headroomMb) +
                " Mo de marge après allocation.");
        }

        // ---- Dossier de jeu ----
        fs::path gameDir = req.gameDir.empty()
                               ? DataStore::instancesRoot() / "default"
                               : fs::path(req.gameDir);
        std::error_code ec;
        fs::create_directories(gameDir, ec);

        // ---- Arguments (ordre C# : jvm, mainClass, jeu) ----
        const std::string classpath =
            info.at("jar").get<std::string>() + ";" +
            [&] {
                std::string joined;
                for (const auto& e : json_string_array(info.at("classpath"))) {
                    if (!joined.empty()) joined += ";";
                    joined += e;
                }
                return joined;
            }();
        const std::string natives = info.at("natives").get<std::string>();
        const std::string mainClass = info.at("mainClass").get<std::string>();

        std::vector<std::string> officialJvm;
        const std::vector<std::string>* officialJvmPtr = nullptr;
        if (info.contains("jvmArgs") && info.at("jvmArgs").is_array()) {
            officialJvm = json_string_array(info.at("jvmArgs"));
            officialJvmPtr = &officialJvm;
        }

        std::vector<std::string> args =
            build_jvm_args(classpath, natives, isForge, officialJvmPtr, ramWanted,
                           req.jvmArgs);
        args.push_back(mainClass);

        const std::string assetsIndex = info.at("assetsIndex").get<std::string>();
        std::string legacy;
        const std::string* legacyPtr = nullptr;
        if (info.contains("minecraftArguments") &&
            info.at("minecraftArguments").is_string()) {
            legacy = info.at("minecraftArguments").get<std::string>();
            legacyPtr = &legacy;
        }
        const bool hasModernArgs = info.value("hasArguments", false);
        const std::string* joinPtr =
            req.joinServer.empty() ? nullptr : &req.joinServer;
        for (const auto& a :
             build_game_args(version, session, assetsIndex, legacyPtr,
                             hasModernArgs, joinPtr))
            args.push_back(a);

        // ---- Demarrage ----
        status("Démarrage de Minecraft...");
        if (cancel.load()) {
            res.cancelled = true;
            return res;
        }
        LaunchInput in;
        in.javaExe = *java;
        in.gameDir = gameDir.string();
        in.mainClass = mainClass;
        in.args = std::move(args);
        auto proc = start_game(in);
        if (!proc)
            throw std::runtime_error("Le processus Java n'a pas pu être démarré.");

        log("Processus Java lancé (PID " + std::to_string(proc->pid) +
            ") — temps total " + std::to_string(elapsed()) + " ms.");

        // En-tete du log de jeu (comme le C# StreamWriter)
        {
            const fs::path gameLog = gameDir / "game-log.txt";
            std::ofstream out(gameLog, std::ios::app);
            if (out)
                out << "\n===== Lancement — " << req.loader + " " << version
                    << " =====\n";
        }
        start_game_log_writer(*proc, gameDir / "game-log.txt");

        res.proc = *proc;
        res.started = true;
        return res;
    } catch (const CancelledError&) {
        log("Lancement annulé par l'utilisateur.");
        res.cancelled = true;
        return res;
    } catch (const std::exception& ex) {
        log(std::string("Erreur de lancement : ") + ex.what());
        res.error = ex.what();
        return res;
    }
}

} // namespace tl

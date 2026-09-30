#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_opengl3.h>
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_opengl.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#ifdef _WIN32
#include <Windows.h>
// SetCurrentProcessExplicitAppUserModelID : identite de l'application pour
// la barre des taches et les notifications (fournie par shell32, deja liee).
#include <shobjidl.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <vector>

#include "datastore.hpp"
#include "maintenance.hpp"
#include "fonts.hpp"
#include "syscolor.hpp"
#include "util_image.hpp"
#include "ui.hpp"

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

int main(int argc, char** argv) {
    SDL_SetMainReady();

#ifdef _WIN32
    // Identite d'application explicite, AVANT toute creation de fenetre.
    //
    // Sans elle, la barre des taches de Windows n'utilise pas l'icone posee
    // sur la fenetre : elle resout l'icone a partir du CHEMIN de
    // l'executable, via le cache du shell — qui gardait l'ancien logo alors
    // que le binaire et la fenetre portaient deja le nouveau.
    //
    // C'est aussi le prerequis des notifications systeme natives : une
    // notification Windows doit etre emise au nom d'un AppUserModelID connu.
    // La chaine doit rester stable dans le temps, sinon Windows considere
    // qu'il s'agit d'une autre application (raccourcis epingles perdus).
    SetCurrentProcessExplicitAppUserModelID(L"TeamLauncher.Minecraft.Launcher");
#endif

    // --portable : donnees a cote de l'exe (avant tout load)
    // --autostart : pose par l'entree de demarrage de session (startup.cpp).
    //   La fenetre s'ouvre alors reduite : surgir au premier plan a chaque
    //   ouverture de session serait insupportable.
    bool autostarted = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--portable") == 0)
            tl::DataStore::isPortable = true;
        else if (std::strcmp(argv[i], "--autostart") == 0)
            autostarted = true;
    }
    tl::DataStore::load();

    // Mise a jour stagee : application differee (l'exe en cours est
    // verrouille sous Windows). On signale seulement ici, AVANT l'UI ;
    // le deploiement passe par updates::install_staged_and_restart().
    if (auto s = tl::updates::staged())
        std::fprintf(stderr, "update pending: v%s\n", s->info.version.c_str());

    // launcher sans console (Windows : stderr reste dispo avec TL_CONSOLE=1 ;
    // Linux : pas de console allouee, rien a masquer).
#ifdef _WIN32
    if (!std::getenv("TL_CONSOLE"))
        if (HWND cons = GetConsoleWindow()) ShowWindow(cons, SW_HIDE);
#endif

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    // Titre sans numero de version : la barre de titre sert a reconnaitre
    // la fenetre, pas a afficher un bulletin de build. La version reste
    // lisible dans Paramètres > Intégrations et dans la page Aide.
    SDL_Window* window = SDL_CreateWindow(
        "Team Launcher",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        960, 620,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // Icone de la fenetre (barre des taches, Alt+Tab, gestionnaire de
    // fenetres). SDL ne reprend PAS l'icone de l'executable : sa classe de
    // fenetre cherche une ressource nommee litteralement « SDL_icon.ico »,
    // ne la trouve pas, et laisse Windows retomber sur ce qu'il a en cache —
    // c'est-a-dire l'ancien logo. Sous Linux il n'y a de toute facon aucune
    // ressource Win32 : la poser ici regle les deux plateformes d'un coup.
    {
        int iw = 0, ih = 0;
        // L'image ronde a fond transparent, la meme que le .desktop.
        // SDL_GetBasePath donne le dossier de l'executable sur les deux
        // plateformes ; on retombe sur un chemin relatif s'il echoue.
        std::vector<unsigned char> px;
        if (char* base = SDL_GetBasePath()) {
            px = tl::image::decode_rgba(
                std::filesystem::path(base) / "assets" / "teamlauncher.png",
                &iw, &ih);
            SDL_free(base);
        }
        if (px.empty())
            px = tl::image::decode_rgba(
                std::filesystem::path("assets") / "teamlauncher.png", &iw, &ih);
        if (!px.empty() && iw > 0 && ih > 0) {
            // Masques RGBA explicites : sans eux l'ordre des octets depend
            // du boutisme de la machine et les couleurs sortent inversees.
            SDL_Surface* s = SDL_CreateRGBSurfaceFrom(
                px.data(), iw, ih, 32, iw * 4, 0x000000FF, 0x0000FF00,
                0x00FF0000, 0xFF000000);
            if (s) {
                SDL_SetWindowIcon(window, s);
                SDL_FreeSurface(s); // ne libere pas `px`, qui vit jusqu'ici
            }
        }
    }

    // Barre de titre accordee au systeme (mode sombre, couleur d'accent).
    // Posee avant la premiere frame pour eviter un clignotement.
    tl::syscolor::poll(window);

    if (autostarted) SDL_MinimizeWindow(window);

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        std::fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_GL_MakeCurrent(window, gl_ctx);
    SDL_GL_SetSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    tl::ui::init(window);

    ImGui_ImplSDL2_InitForOpenGL(window, gl_ctx);
    ImGui_ImplOpenGL3_Init(glsl_version);

    bool running = true;
    const bool dbgSh = std::getenv("TL_DEBUG_SHUTDOWN") != nullptr;

    // Capture automatique (validation visuelle des pages) — cf. plus bas.
    const char* shotPath = std::getenv("TL_SCREENSHOT");
    bool shotTaken = false;
    const Uint32 shotStart = SDL_GetTicks();
    Uint32 shotDelayMs = 6000;
    if (const char* d = std::getenv("TL_SCREENSHOT_DELAY"))
        shotDelayMs = static_cast<Uint32>(std::atof(d) * 1000.0);
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            // SDL_QUIT et WINDOWEVENT_CLOSE arrivent TOUS LES DEUX sur un
            // clic sur la croix (SDL synthetise le premier quand la
            // derniere fenetre se ferme). Les deux passent donc par le
            // meme arbitre, qui decide de quitter ou de reduire.
            if (event.type == SDL_QUIT) {
                if (dbgSh) std::fprintf(stderr, "SH: SDL_QUIT\n");
                if (tl::ui::handle_close_request(window)) running = false;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                if (dbgSh) std::fprintf(stderr, "SH: WINDOWEVENT_CLOSE\n");
                if (tl::ui::handle_close_request(window)) running = false;
            }
            if (event.type == SDL_DROPFILE && event.drop.file) {
                tl::ui::on_drop_file(event.drop.file);
                SDL_free(event.drop.file);
                event.drop.file = nullptr;
            }
        }

        // Changement de police ou d'echelle : l'atlas ne peut pas etre
        // reconstruit au milieu d'une frame (la texture est reference par
        // les listes de dessin en cours). Les parametres posent donc un
        // drapeau, consomme ici, entre deux frames.
        if (tl::ui::fonts::take_rebuild_request()) {
            tl::ui::fonts::build(tl::DataStore::settings.uiFont,
                                 tl::DataStore::settings.fontScale);
            // Force le backend a re-televerser la texture de l'atlas.
            ImGui_ImplOpenGL3_DestroyFontsTexture();
            ImGui_ImplOpenGL3_CreateFontsTexture();
        }

        // L'utilisateur peut basculer clair/sombre ou changer sa couleur
        // d'accentuation pendant que le launcher tourne. Deux lectures de
        // registre par seconde ne coutent rien, et evitent de rester sur
        // une barre de titre depareillee jusqu'au prochain demarrage.
        {
            static Uint32 lastTheme = 0;
            const Uint32 nowMs = SDL_GetTicks();
            if (nowMs - lastTheme > 1000) {
                lastTheme = nowMs;
                tl::syscolor::poll(window);
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        tl::ui::frame(window);

        // Sortie demandee depuis la page Aide : elle ignore le reglage de
        // fermeture, c'est un ordre explicite.
        if (tl::ui::quit_requested()) running = false;

        ImGui::Render();
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.055f, 0.055f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        // TL_SCREENSHOT=<chemin.png> : le launcher se capture lui-meme depuis
        // son propre framebuffer, puis se ferme normalement (le shutdown reste
        // donc exerce). Lecture AVANT SwapWindow : apres l'echange, le contenu
        // du back buffer n'est plus garanti (les modales n'y apparaissaient
        // jamais). On ne photographie jamais l'ecran : aucune autre fenetre
        // ne peut se retrouver dans l'image, et rien ne depend du focus.
        // TL_SCREENSHOT_DELAY=<secondes> (defaut 6) laisse le temps aux
        // chargements reseau d'aboutir.
        if (shotPath && !shotTaken &&
            SDL_GetTicks() >= shotStart + shotDelayMs) {
            shotTaken = true;
            int sw = 0, sh = 0;
            SDL_GL_GetDrawableSize(window, &sw, &sh);
            std::vector<unsigned char> px(static_cast<size_t>(sw) * sh * 4);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            // glReadPixels part d'en bas : flipY remet l'image a l'endroit.
            const bool ok = tl::image::write_png(shotPath, sw, sh, 4, px.data(),
                                                 /*flipY=*/true);
            std::fprintf(stderr, "SHOT %s %dx%d -> %s\n", shotPath, sw, sh,
                         ok ? "OK" : "ECHEC");
            std::fflush(stderr);
            running = false;
        }
        SDL_GL_SwapWindow(window);
    }

    // Annule + joint l'installation en cours avant de tout fermer
    const bool dbg = std::getenv("TL_DEBUG_SHUTDOWN") != nullptr;
    if (dbg) std::fprintf(stderr, "SH: loop exited\n");
    tl::ui::shutdown();
    if (dbg) std::fprintf(stderr, "SH: ui::shutdown done\n");
    tl::DataStore::saveNow();
    if (dbg) std::fprintf(stderr, "SH: saveNow done\n");
    tl::DataStore::shutdown();
    if (dbg) std::fprintf(stderr, "SH: DataStore::shutdown done\n");

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    if (dbg) std::fprintf(stderr, "SH: imgui destroyed\n");
    SDL_GL_DeleteContext(gl_ctx);
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (dbg) std::fprintf(stderr, "SH: complete\n");
    return 0;
}

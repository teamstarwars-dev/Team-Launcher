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
#include <Windows.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "datastore.hpp"
#include "ui.hpp"

#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "6.0.0"
#endif

int main(int argc, char** argv) {
    SDL_SetMainReady();

    // --portable : donnees a cote de l'exe (avant tout load)
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--portable") == 0)
            tl::DataStore::isPortable = true;
    tl::DataStore::load();

    // launcher sans console (stderr reste dispo avec TL_CONSOLE=1)
    if (!std::getenv("TL_CONSOLE"))
        if (HWND cons = GetConsoleWindow()) ShowWindow(cons, SW_HIDE);

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

    SDL_Window* window = SDL_CreateWindow(
        "Team Launcher v" TL_VERSION_STRING,
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        960, 620,
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

    if (!window) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

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
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            if (event.type == SDL_QUIT) {
                if (dbgSh) std::fprintf(stderr, "SH: SDL_QUIT\n");
                running = false;
            }
            if (event.type == SDL_WINDOWEVENT &&
                event.window.event == SDL_WINDOWEVENT_CLOSE &&
                event.window.windowID == SDL_GetWindowID(window)) {
                if (dbgSh) std::fprintf(stderr, "SH: WINDOWEVENT_CLOSE\n");
                running = false;
            }
            if (event.type == SDL_DROPFILE && event.drop.file) {
                tl::ui::on_drop_file(event.drop.file);
                SDL_free(event.drop.file);
                event.drop.file = nullptr;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();

        tl::ui::frame(window);

        ImGui::Render();
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.055f, 0.055f, 0.075f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
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

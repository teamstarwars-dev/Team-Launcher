#pragma once

// UI ImGui (module 3) : sidebar + pages Jouer/Paramètres, machine d'etat
// Idle -> Preparing -> GameRunning, palette fidele a la v5 Avalonia.

struct SDL_Window;

namespace tl::ui {

void init(SDL_Window* window);            // style, police
void frame(SDL_Window* window);           // apres ImGui::NewFrame
void shutdown();                          // annule + joint le worker d'install
void on_drop_file(const char* path);      // SDL_DROPFILE (skins / modpacks)

} // namespace tl::ui

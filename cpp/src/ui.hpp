#pragma once

// UI ImGui (module 3) : sidebar + pages Jouer/Paramètres, machine d'etat
// Idle -> Preparing -> GameRunning, palette fidele a la v5 Avalonia.

struct SDL_Window;

namespace tl::ui {

void init(SDL_Window* window);            // style, police
void frame(SDL_Window* window);           // apres ImGui::NewFrame
void shutdown();                          // annule + joint le worker d'install
void on_drop_file(const char* path);      // SDL_DROPFILE (skins / modpacks)

// Bouton « Fermer » de la fenetre, ou Alt+F4. Retourne true s'il faut
// vraiment quitter, false si le reglage demande de seulement reduire la
// fenetre (la boucle continue alors normalement). Une partie en cours de
// preparation force la reduction : interrompre une installation a moitie
// faite laisse une instance inutilisable.
bool handle_close_request(SDL_Window* window);

// Sortie demandee depuis l'interface (page Aide). La boucle principale la
// consulte apres chaque frame ; elle ignore le reglage de fermeture, car
// c'est un ordre explicite et non un clic sur la croix.
bool quit_requested();
void request_quit();

} // namespace tl::ui

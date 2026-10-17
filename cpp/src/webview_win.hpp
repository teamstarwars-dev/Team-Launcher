#pragma once

// Vue web embarquee (editeur Nova Skin). Implementation WebView2 (Edge) sous
// Windows quand le SDK est present (TL_HAVE_WEBVIEW2) ; partout ailleurs, des
// stubs : available() == false, et la page Nova Skin retombe sur l'ouverture
// dans le navigateur.
//
// Le webview est une fenetre ENFANT posee par-dessus la zone OpenGL. L'appelant
// (page Nova Skin, fil UI) le positionne chaque frame sur le rectangle voulu,
// et le masque en quittant la page.

#include <string>

struct SDL_Window;

namespace tl::webview {

// L'editeur embarque est-il utilisable (SDK compile ET initialisation OK) ?
// Peut rester false un court instant apres ensure() : l'init WebView2 est
// ASYNCHRONE (elle se termine sur un tour de boucle suivant).
bool available();

// Lance (si besoin) la creation du webview enfant de `win` et la navigation
// vers `url` (une seule fois). A appeler sur le fil UI. Rend false si le SDK
// n'est pas compile. L'affichage effectif suit l'init asynchrone.
bool ensure(SDL_Window* win, const std::string& url);

// Positionne et affiche le webview sur un rectangle (pixels, repere fenetre).
// A appeler chaque frame tant que la page Nova Skin est visible.
void show_at(int x, int y, int w, int h);

// Masque le webview (en quittant la page). Le garde en memoire pour un
// reaffichage instantane.
void hide();

// Detruit le webview (fermeture de l'application).
void shutdown();

} // namespace tl::webview

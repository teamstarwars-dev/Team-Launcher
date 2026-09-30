#pragma once

// Barre de titre native accordee au systeme.
//
// La barre du haut — logo, titre, boutons reduire/agrandir/fermer — n'est
// pas dessinee par le launcher : c'est le systeme qui la rend. On ne peut
// donc pas la peindre, seulement lui DIRE quelles couleurs employer.
//
// Windows 10 2004+ / 11 : DwmSetWindowAttribute accepte une couleur de
// legende, une couleur de texte, une couleur de bordure, et un drapeau de
// mode sombre. On y reporte ce que l'utilisateur a choisi dans Windows :
//   - Themes\Personalize\SystemUsesLightTheme -> clair ou sombre ;
//   - Themes\Personalize\ColorPrevalence      -> « afficher la couleur
//     d'accentuation sur les barres de titre » ;
//   - DWM\AccentColor                         -> la couleur en question.
// Quand la prevalence est desactivee, on ne force RIEN : on rend la main a
// Windows, qui dessine sa legende par defaut. Imposer une couleur alors
// que l'utilisateur a demande le contraire serait exactement le defaut
// qu'on cherche a corriger.
//
// Linux : la decoration est dessinee par le gestionnaire de fenetres a
// partir du theme du bureau. Elle est donc DEJA native, et il n'y a rien a
// forcer — une application qui tenterait de la repeindre s'en ecarterait.
// query() y renvoie tout de meme la preference clair/sombre du bureau, qui
// sert au reste de l'interface.

struct SDL_Window;

namespace tl::syscolor {

struct Theme {
    bool valid = false;        // la preference a pu etre lue
    bool dark = false;         // le systeme est en mode sombre
    bool accentOnCaption = false; // accent demande sur les barres de titre
    unsigned accentRgb = 0;    // 0xRRGGBB, valable si accentOnCaption
};

// Etat du systeme, relu a chaque appel (bon marche : deux valeurs de
// registre sous Windows, une variable d'environnement sinon).
Theme query();

// Reporte `t` sur la barre de titre de la fenetre. Sans effet sur les
// versions de Windows qui ignorent ces attributs, et sous Linux.
// Retourne true si quelque chose a ete applique.
bool apply_to_window(SDL_Window* window, const Theme& t);

// Applique si l'etat du systeme a change depuis le dernier appel. A
// appeler de temps en temps depuis la boucle : l'utilisateur peut basculer
// clair/sombre pendant que le launcher tourne.
void poll(SDL_Window* window);

} // namespace tl::syscolor

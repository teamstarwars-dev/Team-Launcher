#pragma once

// Portage de MainForm.EnsureDesktopShortcut — raccourci sur le Bureau,
// cree une seule fois (settings.autoShortcut) : .lnk (Windows, IShellLinkW)
// ou .desktop freedesktop (Linux, XDG_DESKTOP_DIR sinon ~/Desktop).
//
// Le C# passait par l'objet COM WScript.Shell ; ici on utilise directement
// IShellLinkW + IPersistFile : meme resultat, sans dependre du Windows Script
// Host (desactive sur certains postes par politique de securite).

#include <filesystem>
#include <string>

namespace tl {

// Ecrit un .lnk (Windows) ou .desktop (Linux). false = echec (COM
// indisponible, chemin non inscriptible...). Sous Linux un « .lnk » passe en
// entree est reecrit en « .desktop ».
bool create_shortcut(const std::filesystem::path& lnk,
                     const std::filesystem::path& target,
                     const std::filesystem::path& workDir,
                     const std::string& description);

// Chemin du Bureau de l'utilisateur ("" si introuvable).
std::filesystem::path desktop_dir();

// Cree <Bureau>/Team Launcher.lnk (.desktop sous Linux) s'il n'existe pas deja.
// force=false : ne fait rien si settings.autoShortcut est deja vrai (premiere
// ouverture uniquement, comme le C#). force=true : recree a la demande.
// Retourne true si un raccourci a ete ecrit.
bool ensure_desktop_shortcut(bool force = false);

} // namespace tl

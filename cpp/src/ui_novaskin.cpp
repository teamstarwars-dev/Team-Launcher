#include "ui_internal.hpp"

#include "webview_win.hpp"

#include "proc.hpp" // open_detached (POSIX) ; vide sous Windows

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <shellapi.h>
#endif

#include <string>

// ---------------------------------------------------------------------------
// Page Nova Skin (18) — editeur de skin 3D.
//
// Nova Skin est une application WEB. On l'embarque dans le launcher via un
// webview (WebView2 sous Windows, quand le SDK a ete recupere) : la vue est
// une fenetre ENFANT posee sur la zone de contenu. Sans le SDK (ou sur une
// plateforme non geree), on retombe sur l'ouverture dans le navigateur.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

constexpr const char* kNovaUrl = "https://novaskin.me/";

void open_in_browser(const std::string& url) {
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, nullptr, 0);
    if (n <= 0) return;
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, url.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    proc::open_detached(url);
#endif
}

} // namespace

void novaskin_page() {
    // Lance (une fois) la creation du webview embarque ; l'init est asynchrone.
    const bool started = tl::webview::ensure(main_window(), kNovaUrl);

    if (started && tl::webview::available()) {
        // Le webview couvre la zone de contenu. Coordonnees en pixels fenetre
        // (repere ecran ImGui = pixels de la fenetre dans le cas SDL+OpenGL
        // standard ; un ajustement DPI pourra etre necessaire en HiDPI).
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        tl::webview::show_at(static_cast<int>(p.x), static_cast<int>(p.y),
                             static_cast<int>(avail.x), static_cast<int>(avail.y));
        return; // le webview dessine par-dessus : rien d'autre a tracer ici
    }

    // --- Fallback : SDK absent, init en cours, ou init echouee ---
    ImGui::SetCursorPos(ImVec2(24.0f, 24.0f));
    ImGui::TextUnformatted(tr("Nova Skin — éditeur de skin 3D"));
    ImGui::Spacing();
    if (started) {
        ImGui::TextUnformatted(tr("Chargement de l'éditeur embarqué…"));
    } else {
        ImGui::TextWrapped(
            "%s", tr("L'éditeur embarqué n'est pas disponible sur cette "
                     "version du launcher. Ouvre Nova Skin dans ton navigateur."));
    }
    ImGui::Spacing();
    if (ImGui::Button(tr("Ouvrir Nova Skin dans le navigateur"), ImVec2(340, 36)))
        open_in_browser(kNovaUrl);
}

} // namespace tl::ui

#include "webview_win.hpp"

#if defined(TL_HAVE_WEBVIEW2) && defined(_WIN32)

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <wrl.h>
// WebView2.h declare l'API COM du moteur Edge.
#include <WebView2.h>

#include <SDL.h>
#include <SDL_syswm.h>

#include "datastore.hpp" // dossier de donnees utilisateur du webview

#include <string>

using Microsoft::WRL::Callback;
using Microsoft::WRL::ComPtr;

namespace tl::webview {

namespace {

struct State {
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    bool comInit = false;
    bool initStarted = false;
    bool ready = false;
    std::wstring pendingUrl;
    RECT lastBounds{0, 0, 0, 0};
};
State g;

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

HWND hwnd_of(SDL_Window* win) {
    if (!win) return nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(win, &info)) return nullptr;
    return info.info.win.window;
}

} // namespace

bool available() { return g.ready; }

bool ensure(SDL_Window* win, const std::string& url) {
    if (g.initStarted) {
        // Déjà lancé : si l'URL change et que le webview est prêt, on navigue.
        const std::wstring w = utf8_to_wide(url);
        if (g.ready && g.webview && !w.empty() && w != g.pendingUrl) {
            g.pendingUrl = w;
            g.webview->Navigate(w.c_str());
        }
        return true;
    }
    g.hwnd = hwnd_of(win);
    if (!g.hwnd) return false;
    g.pendingUrl = utf8_to_wide(url);

    if (!g.comInit) {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        g.comInit = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }

    // Dossier de données du webview (cache, cookies) sous le dossier du launcher.
    const std::wstring dataDir = utf8_to_wide((DataStore::dir() / "webview2").string());

    const HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, dataDir.empty() ? nullptr : dataDir.c_str(), nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT r, ICoreWebView2Environment* env) -> HRESULT {
                if (FAILED(r) || !env) return r;
                return env->CreateCoreWebView2Controller(
                    g.hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT r2, ICoreWebView2Controller* ctrl) -> HRESULT {
                            if (FAILED(r2) || !ctrl) return r2;
                            g.controller = ctrl;
                            g.controller->get_CoreWebView2(&g.webview);
                            // Caché tant que la page Nova Skin n'est pas montrée.
                            g.controller->put_IsVisible(FALSE);
                            if (g.webview && !g.pendingUrl.empty())
                                g.webview->Navigate(g.pendingUrl.c_str());
                            g.ready = true;
                            return S_OK;
                        })
                        .Get());
            })
            .Get());

    g.initStarted = SUCCEEDED(hr);
    return g.initStarted;
}

void show_at(int x, int y, int w, int h) {
    if (!g.ready || !g.controller) return;
    const RECT b{x, y, x + w, y + h};
    if (b.left != g.lastBounds.left || b.top != g.lastBounds.top ||
        b.right != g.lastBounds.right || b.bottom != g.lastBounds.bottom) {
        g.controller->put_Bounds(b);
        g.lastBounds = b;
    }
    g.controller->put_IsVisible(TRUE);
}

void hide() {
    if (g.controller) g.controller->put_IsVisible(FALSE);
}

void shutdown() {
    if (g.controller) g.controller->Close();
    g.webview.Reset();
    g.controller.Reset();
    g.ready = false;
    g.initStarted = false;
}

} // namespace tl::webview

#else // Pas de SDK WebView2 (ou plateforme non Windows) : stubs.

namespace tl::webview {
bool available() { return false; }
bool ensure(SDL_Window*, const std::string&) { return false; }
void show_at(int, int, int, int) {}
void hide() {}
void shutdown() {}
} // namespace tl::webview

#endif

#include "syscolor.hpp"

#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_syswm.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#else
#include <cctype>
#include <cstdlib>
#include <string>
#endif

namespace tl::syscolor {

namespace {

#ifdef _WIN32

// Attributs DwmSetWindowAttribute. Definis a la main : les en-tetes du
// SDK livre avec le compilateur ne les contiennent pas tous, et un build
// qui compile ici doit compiler chez les autres.
constexpr DWORD kUseImmersiveDarkMode = 20; // BOOL
constexpr DWORD kBorderColor = 34;          // COLORREF
constexpr DWORD kCaptionColor = 35;         // COLORREF
constexpr DWORD kTextColor = 36;            // COLORREF
// Valeur sentinelle : « reprends ta couleur par defaut ».
constexpr COLORREF kColorDefault = 0xFFFFFFFF;

using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

// dwmapi.dll est present sur tout Windows encore supporte, mais on la
// charge quand meme a la demande : cela evite de lier une bibliotheque
// d'import de plus, et une absence devient un « on ne fait rien » plutot
// qu'un refus de demarrer.
DwmSetWindowAttributeFn dwm_set() {
    static DwmSetWindowAttributeFn fn = [] {
        HMODULE m = LoadLibraryW(L"dwmapi.dll");
        if (!m) return static_cast<DwmSetWindowAttributeFn>(nullptr);
        return reinterpret_cast<DwmSetWindowAttributeFn>(
            reinterpret_cast<void*>(GetProcAddress(m, "DwmSetWindowAttribute")));
    }();
    return fn;
}

// Valeur DWORD du registre. false = absente ou d'un autre type.
bool reg_dword(HKEY root, const wchar_t* path, const wchar_t* name,
               DWORD& out) {
    HKEY key{};
    if (RegOpenKeyExW(root, path, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = sizeof(DWORD), value = 0;
    const LSTATUS st = RegQueryValueExW(key, name, nullptr, &type,
                                        reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_DWORD) return false;
    out = value;
    return true;
}

#else

// Preference clair/sombre du bureau. Pas d'appel a gsettings : lancer un
// processus a chaque sondage serait absurde. On lit les conventions que
// les environnements de bureau exposent dans l'environnement du
// processus ; a defaut, on ne se prononce pas.
bool posix_dark(bool& darkOut) {
    struct Probe {
        const char* var;
        const char* needle;
    };
    static const Probe kProbes[] = {
        {"GTK_THEME", "dark"},
        {"QT_STYLE_OVERRIDE", "dark"},
        {"COLOR_SCHEME", "dark"},
    };
    for (const auto& p : kProbes) {
        const char* v = std::getenv(p.var);
        if (!v) continue;
        std::string s(v);
        for (char& c : s) c = static_cast<char>(tolower((unsigned char)c));
        if (s.find(p.needle) != std::string::npos) {
            darkOut = true;
            return true;
        }
    }
    return false;
}

#endif

// Noir ou blanc, selon ce qui se lit le mieux sur le fond donne.
unsigned readable_on(unsigned rgb) {
    const float r = ((rgb >> 16) & 0xFF) / 255.0f;
    const float g = ((rgb >> 8) & 0xFF) / 255.0f;
    const float b = (rgb & 0xFF) / 255.0f;
    const float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    return lum > 0.55f ? 0x000000u : 0xFFFFFFu;
}

bool same(const Theme& a, const Theme& b) {
    return a.valid == b.valid && a.dark == b.dark &&
           a.accentOnCaption == b.accentOnCaption &&
           a.accentRgb == b.accentRgb;
}

} // namespace

Theme query() {
    Theme t;
#ifdef _WIN32
    constexpr const wchar_t* kPersonalize =
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    constexpr const wchar_t* kDwm = L"Software\\Microsoft\\Windows\\DWM";

    DWORD v = 0;
    // SystemUsesLightTheme, et non AppsUseLightTheme : c'est bien celle-ci
    // qui gouverne les barres de titre et la barre des taches. Les deux
    // peuvent differer, et beaucoup de monde a « applications claires,
    // systeme sombre ».
    if (reg_dword(HKEY_CURRENT_USER, kPersonalize, L"SystemUsesLightTheme", v)) {
        t.valid = true;
        t.dark = v == 0;
    }
    DWORD prevalence = 0;
    if (reg_dword(HKEY_CURRENT_USER, kPersonalize, L"ColorPrevalence",
                  prevalence) &&
        prevalence != 0) {
        DWORD accent = 0;
        // AccentColor est ecrit en 0xAABBGGRR (composantes inversees par
        // rapport a l'usage courant) : on remet R en tete.
        if (reg_dword(HKEY_CURRENT_USER, kDwm, L"AccentColor", accent)) {
            const unsigned b = (accent >> 16) & 0xFF;
            const unsigned g = (accent >> 8) & 0xFF;
            const unsigned r = accent & 0xFF;
            t.accentRgb = (r << 16) | (g << 8) | b;
            t.accentOnCaption = true;
            t.valid = true;
        }
    }
#else
    bool dark = false;
    if (posix_dark(dark)) {
        t.valid = true;
        t.dark = dark;
    }
#endif
    return t;
}

bool apply_to_window(SDL_Window* window, const Theme& t) {
#ifdef _WIN32
    if (!window || !t.valid) return false;
    auto set = dwm_set();
    if (!set) return false;

    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info)) return false;
    if (info.subsystem != SDL_SYSWM_WINDOWS) return false;
    HWND hwnd = info.info.win.window;
    if (!hwnd) return false;

    const BOOL dark = t.dark ? TRUE : FALSE;
    set(hwnd, kUseImmersiveDarkMode, &dark, sizeof(dark));

    // Prevalence desactivee : on REND la legende a Windows au lieu de lui
    // imposer une teinte. C'est le reglage de l'utilisateur, pas le notre.
    COLORREF caption = kColorDefault;
    COLORREF text = kColorDefault;
    COLORREF border = kColorDefault;
    if (t.accentOnCaption) {
        const unsigned rgb = t.accentRgb;
        // COLORREF est 0x00BBGGRR : encore une inversion.
        caption = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
        const unsigned txt = readable_on(rgb);
        text = RGB((txt >> 16) & 0xFF, (txt >> 8) & 0xFF, txt & 0xFF);
        border = caption;
    }
    set(hwnd, kCaptionColor, &caption, sizeof(caption));
    set(hwnd, kTextColor, &text, sizeof(text));
    set(hwnd, kBorderColor, &border, sizeof(border));
    return true;
#else
    // Le gestionnaire de fenetres decore deja selon le theme du bureau :
    // il n'y a rien a lui demander, et rien a corriger.
    (void)window;
    (void)t;
    return false;
#endif
}

void poll(SDL_Window* window) {
    static Theme last;
    static bool first = true;
    const Theme now = query();
    if (!first && same(now, last)) return;
    first = false;
    last = now;
    apply_to_window(window, now);
}

} // namespace tl::syscolor

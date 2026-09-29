#pragma once

// Conversion UTF-8 <-> wide natif : UTF-16 (Win32) / UTF-32 (POSIX).
// Sans dependance (ni iconv ni codecvt) : l'UTF-8 est decode/encode a la main.
// Surrogates et scalaires hors plage (> U+10FFFF) : ignores, jamais produits.

#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#else
#include <strings.h> // strcasecmp
#endif

namespace tl {

// Comparaison insensible a la casse : _stricmp (Win32) / strcasecmp (POSIX).
// Centralise les sites restants (pack_share, worldsync, ui_*) ; les modules
// portes avant ont leur helper local equivalent (datastore, moddev...).
inline int strCaseCmp(const char* a, const char* b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return ::strcasecmp(a, b);
#endif
}

#ifdef _WIN32

inline std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

inline std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

#else // POSIX : wchar_t 32 bits (UTF-32 natif)

inline std::wstring utf8_to_wide(const std::string& s) {
    std::wstring w;
    w.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        char32_t cp = 0;
        size_t len = 0;
        if (c < 0x80) {
            cp = c;
            len = 1;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1F;
            len = 2;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0F;
            len = 3;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07;
            len = 4;
        } else {
            ++i;
            continue; // octet de tete invalide : saute
        }
        if (i + len > s.size()) break; // queue tronquee : stop
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char d = static_cast<unsigned char>(s[i + k]);
            if ((d & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) {
            ++i;
            continue; // continuation invalide : resynchronise
        }
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            i += len;
            continue; // surrogate ou hors plage : ignore
        }
        w.push_back(static_cast<wchar_t>(cp));
        i += len;
    }
    return w;
}

inline std::string wide_to_utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    std::string s;
    for (const wchar_t* p = w; *p; ++p) {
        const char32_t cp = static_cast<char32_t>(*p);
        if (cp < 0x80) {
            s.push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
            s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return s;
}

#endif

} // namespace tl

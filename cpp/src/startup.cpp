#include "startup.hpp"

#include <filesystem>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace tl::startup {

namespace {

#ifdef _WIN32

constexpr const wchar_t* kRunKey =
    L"Software\Microsoft\Windows\CurrentVersion\Run";
constexpr const wchar_t* kValueName = L"TeamLauncher";

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
                                        static_cast<int>(w.size()), nullptr, 0,
                                        nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
                          s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring exe_path_w() {
    // MAX_PATH ne suffit pas toujours (chemins longs actives) : on agrandit
    // tant que l'API signale la troncature.
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(),
                                           static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return buf;
        }
        if (buf.size() >= 32768) return {};
        buf.resize(buf.size() * 2);
    }
}

// Ligne de commande inscrite dans la cle Run. Les guillemets sont
// indispensables : sans eux, un chemin contenant un espace (« Program
// Files ») est coupe et Windows lance le mauvais programme, ou rien.
std::wstring run_command() {
    const std::wstring exe = exe_path_w();
    if (exe.empty()) return {};
    return L"\"" + exe + L"\" --autostart";
}

#else // POSIX

fs::path autostart_file() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const char* home = std::getenv("HOME");
    fs::path base;
    if (xdg && *xdg)
        base = fs::path(xdg);
    else if (home && *home)
        base = fs::path(home) / ".config";
    else
        return {};
    return base / "autostart" / "teamlauncher.desktop";
}

std::string exe_path_posix() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::string(buf);
}

#endif

} // namespace

std::string exe_path_utf8() {
#ifdef _WIN32
    return to_utf8(exe_path_w());
#else
    return exe_path_posix();
#endif
}

bool autostart_supported() {
#ifdef _WIN32
    return !exe_path_w().empty();
#else
    return !autostart_file().empty() && !exe_path_posix().empty();
#endif
}

bool autostart_enabled() {
#ifdef _WIN32
    HKEY key{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    DWORD type = 0, size = 0;
    const LSTATUS st =
        RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
    RegCloseKey(key);
    return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
#else
    std::error_code ec;
    const fs::path f = autostart_file();
    return !f.empty() && fs::is_regular_file(f, ec);
#endif
}

bool set_autostart(bool on, std::string* errOut) {
    auto fail = [&](const char* msg) {
        if (errOut) *errOut = msg;
        return false;
    };
#ifdef _WIN32
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return fail("Clé de démarrage inaccessible dans le registre.");
    LSTATUS st;
    if (on) {
        const std::wstring cmd = run_command();
        if (cmd.empty()) {
            RegCloseKey(key);
            return fail("Chemin de l'exécutable introuvable.");
        }
        st = RegSetValueExW(
            key, kValueName, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        st = RegDeleteValueW(key, kValueName);
        // Rien a supprimer = resultat demande atteint, pas une erreur.
        if (st == ERROR_FILE_NOT_FOUND) st = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) return fail("Écriture dans le registre refusée.");
    return true;
#else
    const fs::path f = autostart_file();
    if (f.empty()) return fail("Dossier de configuration introuvable.");
    std::error_code ec;
    if (!on) {
        fs::remove(f, ec);
        if (ec) return fail("Suppression du fichier de démarrage impossible.");
        return true;
    }
    const std::string exe = exe_path_posix();
    if (exe.empty()) return fail("Chemin de l'exécutable introuvable.");
    fs::create_directories(f.parent_path(), ec);
    std::ofstream out(f, std::ios::binary | std::ios::trunc);
    if (!out) return fail("Écriture du fichier de démarrage impossible.");
    out << "[Desktop Entry]\n"
        << "Type=Application\n"
        << "Name=Team Launcher\n"
        << "Comment=Launcher Minecraft\n"
        << "Exec=\"" << exe << "\" --autostart\n"
        << "Terminal=false\n"
        << "X-GNOME-Autostart-enabled=true\n";
    out.close();
    if (!out) return fail("Écriture du fichier de démarrage interrompue.");
    return true;
#endif
}

} // namespace tl::startup

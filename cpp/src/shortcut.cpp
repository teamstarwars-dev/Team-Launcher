#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#else
// Etape 5 (Linux) : raccourci freedesktop (.desktop), pas de COM.
#include <cstdlib>
#include <fstream>
#include <string>
#include <unistd.h> // readlink (/proc/self/exe)
#endif

#include "shortcut.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line

namespace fs = std::filesystem;

namespace tl {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<size_t>(n - 1));
    return w;
}

// CoInitializeEx par appel : la fonction peut etre appelee depuis n'importe
// quel thread, et RPC_E_CHANGED_MODE signifie « deja initialise autrement »,
// ce qui reste exploitable.
struct ComScope {
    bool needUninit = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        needUninit = SUCCEEDED(hr);
    }
    ~ComScope() {
        if (needUninit) CoUninitialize();
    }
};

fs::path exe_path() {
    wchar_t buf[MAX_PATH] = L"";
    const DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return fs::path(buf);
}

} // namespace
#else // POSIX

// Lit XDG_DESKTOP_DIR dans ~/.config/user-dirs.dirs (avec $HOME et guillemets).
static std::string user_dirs_value(const fs::path& cfg, const char* key) {
    std::ifstream in(cfg);
    if (!in) return {};
    std::string line;
    const size_t klen = std::char_traits<char>::length(key);
    while (std::getline(in, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos || line[b] == '#') continue;
        if (line.compare(b, klen, key) != 0) continue;
        std::string v = line.substr(b + klen);
        while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r'))
            v.pop_back();
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
            v = v.substr(1, v.size() - 2);
        return v;
    }
    return {};
}

static std::string expand_home(const std::string& v, const std::string& home) {
    if (v.rfind("$HOME", 0) == 0) return home + v.substr(5);
    if (v.rfind("${HOME}", 0) == 0) return home + v.substr(7);
    return v;
}

fs::path exe_path() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return fs::path(buf);
}

} // namespace

// Quoting minimal pour Exec=/Path= (.desktop) : guillemets si espace ou quote.
static std::string desktop_quote(const std::string& s) {
    if (s.find_first_of(" \"'$`\\") == std::string::npos) return s;
    return "\"" + s + "\"";
}
#endif // _WIN32 / POSIX

fs::path desktop_dir() {
#ifdef _WIN32
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) return {};
    fs::path out(p);
    CoTaskMemFree(p);
    return out;
#else
    // Bureau XDG : XDG_DESKTOP_DIR de ~/.config/user-dirs.dirs, sinon ~/Desktop.
    const char* home = std::getenv("HOME");
    if (!home || !*home) return {};
    const std::string hs(home);
    const std::string v = expand_home(
        user_dirs_value(fs::path(hs) / ".config" / "user-dirs.dirs", "XDG_DESKTOP_DIR="), hs);
    if (!v.empty() && v != hs) return fs::path(v);
    return fs::path(hs) / "Desktop"; // absent, vide ou « desactive » ($HOME seul)
#endif
}

bool create_shortcut(const fs::path& lnk, const fs::path& target,
                     const fs::path& workDir, const std::string& description) {
    if (target.empty()) return false;
#ifdef _WIN32
    ComScope com;

    IShellLinkW* link = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IShellLinkW, reinterpret_cast<void**>(&link))))
        return false;

    bool ok = false;
    link->SetPath(target.wstring().c_str());
    if (!workDir.empty()) link->SetWorkingDirectory(workDir.wstring().c_str());
    if (!description.empty()) link->SetDescription(widen(description).c_str());
    // Icone : celle de l'exe lui-meme.
    link->SetIconLocation(target.wstring().c_str(), 0);

    IPersistFile* file = nullptr;
    if (SUCCEEDED(link->QueryInterface(IID_IPersistFile,
                                       reinterpret_cast<void**>(&file)))) {
        std::error_code ec;
        if (lnk.has_parent_path()) fs::create_directories(lnk.parent_path(), ec);
        ok = SUCCEEDED(file->Save(lnk.wstring().c_str(), TRUE));
        file->Release();
    }
    link->Release();
    return ok;
#else
    // Fichier .desktop freedesktop. Un eventuel « .lnk » (appelant Windows)
    // est reecrit en « .desktop » : un .desktop nomme .lnk ne serait reconnu
    // par aucun lanceur. Pas de cle Icon : le seul asset est un .ico Windows,
    // invalide ici (un chemin d'exe n'est pas une icone valide non plus).
    fs::path out = lnk;
    if (out.extension() == ".lnk") out.replace_extension(".desktop");
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    std::ofstream f(out, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << "[Desktop Entry]\nType=Application\nVersion=1.0\nName=Team Launcher\n";
    if (!description.empty()) f << "Comment=" << description << "\n";
    f << "Exec=" << desktop_quote(target.string()) << "\n";
    if (!workDir.empty()) f << "Path=" << desktop_quote(workDir.string()) << "\n";
    f << "Terminal=false\nCategories=Game;\n";
    f.close();
    if (f.fail()) {
        fs::remove(out, ec);
        return false;
    }
    // +x : les gestionnaires de fichiers exigent le bit executable pour
    // proposer le lancement (un echec ici n'invalide pas le fichier).
    fs::permissions(out, fs::perms::owner_exec, fs::perm_options::add, ec);
    return true;
#endif
}

bool ensure_desktop_shortcut(bool force) {
    // Premiere ouverture uniquement, comme le C#.
    if (!force && DataStore::settings.autoShortcut) return false;

    bool written = false;
    const fs::path desk = desktop_dir();
    if (desk.empty()) {
        log_line("Raccourci bureau : dossier Bureau introuvable.");
    } else {
#ifdef _WIN32
        const fs::path lnk = desk / "Team Launcher.lnk";
#else
        const fs::path lnk = desk / "Team Launcher.desktop";
#endif
        std::error_code ec;
        if (fs::exists(lnk, ec)) {
            // Deja la : on ne l'ecrase pas (l'utilisateur a pu le deplacer
            // ou le renommer volontairement).
            if (!force) {
                DataStore::settings.autoShortcut = true;
                DataStore::save();
                return false;
            }
        }
        const fs::path exe = exe_path();
        written = create_shortcut(lnk, exe, exe.parent_path(),
                                  "Lanceur Minecraft léger");
        log_line(written ? "Raccourci bureau créé : " + lnk.string()
                         : "Raccourci bureau : création impossible.");
    }

    // Marque la tentative faite meme en cas d'echec : inutile de reessayer a
    // chaque demarrage (le C# faisait de meme, l'echec etant silencieux).
    if (!DataStore::settings.autoShortcut) {
        DataStore::settings.autoShortcut = true;
        DataStore::save();
    }
    return written;
}

} // namespace tl

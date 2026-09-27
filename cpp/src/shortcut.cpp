#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "shortcut.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // log_line

namespace fs = std::filesystem;

namespace tl {

namespace {

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

fs::path desktop_dir() {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) return {};
    fs::path out(p);
    CoTaskMemFree(p);
    return out;
}

bool create_shortcut(const fs::path& lnk, const fs::path& target,
                     const fs::path& workDir, const std::string& description) {
    if (target.empty()) return false;
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
}

bool ensure_desktop_shortcut(bool force) {
    // Premiere ouverture uniquement, comme le C#.
    if (!force && DataStore::settings.autoShortcut) return false;

    bool written = false;
    const fs::path desk = desktop_dir();
    if (desk.empty()) {
        log_line("Raccourci bureau : dossier Bureau introuvable.");
    } else {
        const fs::path lnk = desk / "Team Launcher.lnk";
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

// Boites de dialogue fichier + ouverture dossiers (extrait de ui.cpp, etape 5).
// Windows : commdlg/shell (inchangé). POSIX : zenity puis kdialog en
// sous-processus (proc::run_capture, modal comme GetOpenFileName), xdg-open
// pour l'ouverture (fire-and-forget comme ShellExecuteW). Sans zenity ni
// kdialog les sélecteurs rendent nullopt (dégradation propre).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "proc.hpp" // run_capture/spawn (POSIX) ; vide sous Windows
#include "util_str.hpp" // wide_to_utf8 (UTF-16 Win32 / UTF-32 POSIX)

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shellapi.h>
#endif

namespace fs = std::filesystem;

namespace tl::ui {

std::string wstr_to_utf8(const wchar_t* w) { return tl::wide_to_utf8(w); }

// --- Sélecteur générique ----------------------------------------------------
// "Nom (*.a *.b)" + "*.a *.b" (motifs espaces). Utilisé par les pages
// (packs/settings/skins) ; les pickers historiques ci-dessous restent tels
// quels (chemins éprouvés).

#ifdef _WIN32
namespace {

// Convertit en wide en conservant les NUL médians (filtres doubles).
std::wstring widen_n(const std::string& s) {
    if (s.empty()) return {};
    const int n =
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// "Nom (*.a *.b)" + "*.a *.b" -> "Nom (*.a *.b)\0*.a;*.b\0\0" (commdlg veut
// des ';' et une double fin NUL).
std::wstring build_filter(const std::string& name, const std::string& pats) {
    std::string p = pats;
    for (char& c : p)
        if (c == ' ') c = ';';
    std::string f = name;
    f.push_back('\0');
    f += p;
    f.push_back('\0');
    std::wstring w = widen_n(f);
    w.push_back(L'\0');
    return w;
}

std::wstring widen_title(const std::string& t) { return widen_n(t); }

} // namespace

std::optional<std::string> pick_file_open(const std::string& title,
                                          const std::string& filterName,
                                          const std::string& filterPat,
                                          const std::string& startFile) {
    std::vector<wchar_t> file(4096, L'\0');
    if (!startFile.empty()) {
        const std::wstring init = widen_n(startFile);
        std::copy_n(init.c_str(), (std::min)(init.size() + 1, file.size() - 1),
                    file.data());
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    const std::wstring filter = build_filter(filterName, filterPat);
    ofn.lpstrFilter = filter.c_str();
    const std::wstring wt = widen_title(title);
    if (!wt.empty()) ofn.lpstrTitle = wt.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file.data());
}

std::vector<std::string> pick_files_open(const std::string& title,
                                         const std::string& filterName,
                                         const std::string& filterPat,
                                         const std::string& startDir) {
    std::vector<wchar_t> buf(32768, L'\0');
    if (!startDir.empty()) {
        const std::wstring init = widen_n(startDir);
        std::copy_n(init.c_str(), (std::min)(init.size() + 1, buf.size() - 1),
                    buf.data());
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = static_cast<DWORD>(buf.size());
    const std::wstring filter = build_filter(filterName, filterPat);
    ofn.lpstrFilter = filter.c_str();
    const std::wstring wt = widen_title(title);
    if (!wt.empty()) ofn.lpstrTitle = wt.c_str();
    ofn.Flags = OFN_ALLOWMULTISELECT | OFN_EXPLORER | OFN_FILEMUSTEXIST |
                OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    std::vector<std::string> out;
    const wchar_t* p = buf.data();
    const std::wstring first(p);
    if (first.empty()) return out;
    p += first.size() + 1;
    if (*p == L'\0') { // fichier unique
        out.push_back(wstr_to_utf8(first.c_str()));
        return out;
    }
    while (*p != L'\0') {
        const std::wstring f(p);
        p += f.size() + 1;
        out.push_back((fs::path(first) / f).string());
    }
    return out;
}

std::optional<std::string> pick_file_save(const std::string& title,
                                          const std::string& filterName,
                                          const std::string& filterPat,
                                          const std::string& defaultName,
                                          const std::string& defExt) {
    std::vector<wchar_t> file(4096, L'\0');
    if (!defaultName.empty()) {
        const std::wstring init = widen_n(defaultName);
        std::copy_n(init.c_str(), (std::min)(init.size() + 1, file.size() - 1),
                    file.data());
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file.data();
    ofn.nMaxFile = static_cast<DWORD>(file.size());
    const std::wstring filter = build_filter(filterName, filterPat);
    ofn.lpstrFilter = filter.c_str();
    const std::wstring wt = widen_title(title);
    if (!wt.empty()) ofn.lpstrTitle = wt.c_str();
    const std::wstring we = widen_n(defExt);
    if (!we.empty()) ofn.lpstrDefExt = we.c_str();
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file.data());
}

#endif // _WIN32 (générique) — les pickers historiques suivent, même garde.

#ifdef _WIN32

std::optional<std::string> pick_javaw() {
    wchar_t file[MAX_PATH] = L"javaw.exe";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"javaw.exe\0javaw.exe\0Tous les fichiers\0*.*\0";
    ofn.lpstrTitle = L"Choisir javaw.exe";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
}

void open_in_explorer(const std::filesystem::path& p) {
    const std::wstring w = L"\"" + p.wstring() + L"\"";
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::optional<std::string> pick_zip_open() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Archive ZIP (*.zip)\0*.zip\0Tous les fichiers\0*.*\0";
    ofn.lpstrTitle = L"Importer une instance ZIP";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
}

// Modeles 3D : .bbmodel, modele Java .json, geometrie Bedrock .geo.json.
// Le C# proposait aussi « *.obj », format qu aucun code ne lisait.
std::optional<std::string> pick_model_file() {
    wchar_t file[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter =
        L"Modeles 3D (*.bbmodel;*.json)\0*.bbmodel;*.json\0Tous les fichiers\0*.*\0";
    ofn.lpstrTitle = L"Ouvrir un modele 3D";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
}

std::optional<std::string> pick_zip_save(const std::string& defaultName) {
    wchar_t file[MAX_PATH] = L"";
    MultiByteToWideChar(CP_UTF8, 0, defaultName.c_str(), -1, file, MAX_PATH);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"Archive ZIP (*.zip)\0*.zip\0";
    ofn.lpstrTitle = L"Exporter l'instance";
    ofn.lpstrDefExt = L"zip";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return std::nullopt;
    return wstr_to_utf8(file);
}

std::optional<std::string> pick_folder(const wchar_t* title) {
    BROWSEINFOW bi{};
    wchar_t disp[MAX_PATH] = L"";
    bi.pszDisplayName = disp;
    bi.lpszTitle = title;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NONEWFOLDERBUTTON;
    const PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return std::nullopt;
    wchar_t path[MAX_PATH] = L"";
    const BOOL ok = SHGetPathFromIDListW(pidl, path);
    ILFree(pidl);
    if (!ok) return std::nullopt;
    return wstr_to_utf8(path);
}

#else // POSIX : zenity puis kdialog

namespace {

std::string trim_end(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r' || s[e - 1] == ' ' ||
                     s[e - 1] == '\t'))
        --e;
    return s.substr(0, e);
}

// Execute un sélecteur externe, modal (attend l'utilisateur, comme
// GetOpenFileName). nullopt = annulé, erreur ou outil absent.
std::optional<std::string> run_tool(const std::string& exe,
                                    const std::vector<std::string>& args) {
    std::vector<std::string> argv{exe};
    argv.insert(argv.end(), args.begin(), args.end());
    const auto r = proc::run_capture(exe, argv, "", -1);
    if (r.code != 0) return std::nullopt;
    const std::string p = trim_end(r.output);
    if (p.empty()) return std::nullopt;
    return p;
}

// Ouverture de fichier : zenity puis kdialog.
std::optional<std::string> pick_open(const std::string& title,
                                     const std::string& filterName,
                                     const std::string& filterPat,
                                     const std::string& startFile) {
    {
        std::vector<std::string> a{"--file-selection", "--title=" + title};
        if (!filterName.empty())
            a.push_back("--file-filter=" + filterName + " | " + filterPat);
        if (!startFile.empty()) a.push_back("--filename=" + startFile);
        if (auto r = run_tool("zenity", a)) return r;
    }
    {
        const std::string filter =
            filterName.empty() ? "*" : (filterPat + "|" + filterName + " (" + filterPat + ")");
        std::vector<std::string> a{"--getopenfilename",
                                   startFile.empty() ? "." : startFile, filter,
                                   "--title", title};
        if (auto r = run_tool("kdialog", a)) return r;
    }
    return std::nullopt;
}

std::optional<std::string> pick_save(const std::string& title,
                                     const std::string& startFile,
                                     const std::string& filterName,
                                     const std::string& filterPat) {
    {
        std::vector<std::string> a{"--file-selection", "--save", "--confirm-overwrite",
                                   "--title=" + title};
        if (!filterName.empty())
            a.push_back("--file-filter=" + filterName + " | " + filterPat);
        if (!startFile.empty()) a.push_back("--filename=" + startFile);
        if (auto r = run_tool("zenity", a)) return r;
    }
    {
        std::vector<std::string> a{"--getsavefilename",
                                   startFile.empty() ? "." : startFile};
        if (!filterName.empty())
            a.push_back(filterPat + "|" + filterName + " (" + filterPat + ")");
        a.push_back("--title");
        a.push_back(title);
        if (auto r = run_tool("kdialog", a)) return r;
    }
    return std::nullopt;
}

// Découpe la sortie multi-fichiers (une ligne par chemin).
std::vector<std::string> split_lines(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        std::string line = trim_end(s.substr(i, e - i));
        if (!line.empty()) out.push_back(line);
        i = e + 1;
    }
    return out;
}

std::vector<std::string> pick_multi(const std::string& title,
                                    const std::string& filterName,
                                    const std::string& filterPat,
                                    const std::string& startDir) {
    {
        std::vector<std::string> a{"--file-selection", "--multiple",
                                   "--separator=\n", "--title=" + title};
        if (!filterName.empty())
            a.push_back("--file-filter=" + filterName + " | " + filterPat);
        if (!startDir.empty()) a.push_back("--filename=" + startDir);
        std::vector<std::string> argv{"zenity"};
        argv.insert(argv.end(), a.begin(), a.end());
        const auto r = proc::run_capture("zenity", argv, "", -1);
        if (r.code == 0) {
            const auto v = split_lines(r.output);
            if (!v.empty()) return v;
        }
    }
    {
        const std::string filter =
            filterName.empty() ? "*" : (filterPat + "|" + filterName + " (" + filterPat + ")");
        std::vector<std::string> a{"--getopenfilename",
                                   startDir.empty() ? "." : startDir, filter,
                                   "--title", title, "--multiple",
                                   "--separate-output"};
        std::vector<std::string> argv{"kdialog"};
        argv.insert(argv.end(), a.begin(), a.end());
        const auto r = proc::run_capture("kdialog", argv, "", -1);
        if (r.code == 0) {
            const auto v = split_lines(r.output);
            if (!v.empty()) return v;
        }
    }
    return {};
}

std::optional<std::string> pick_dir(const std::string& title) {
    if (auto r = run_tool("zenity",
                          {"--file-selection", "--directory", "--title=" + title}))
        return r;
    if (auto r = run_tool("kdialog", {"--getexistingdirectory", ".", "--title", title}))
        return r;
    return std::nullopt;
}

// Comme lpstrDefExt : ajoute l'extension si le chemin n'en a pas.
std::string ensure_ext(std::string p, const std::string& ext) {
    if (ext.empty()) return p;
    const std::string dot = ext[0] == '.' ? ext : "." + ext;
    const size_t slash = p.find_last_of('/');
    const size_t dotpos = p.find_last_of('.');
    if (dotpos == std::string::npos || (slash != std::string::npos && dotpos < slash))
        p += dot;
    return p;
}

} // namespace

std::optional<std::string> pick_javaw() {
    return pick_open("Choisir java", "java", "java*", "/usr/bin/java");
}

void open_in_explorer(const std::filesystem::path& p) {
    // Fire-and-forget comme ShellExecuteW : pas d'attente (l'UI ne bloque pas).
    proc::open_detached(p.string());
}

std::optional<std::string> pick_zip_open() {
    return pick_open("Importer une instance ZIP", "Archive ZIP (*.zip)", "*.zip", "");
}

std::optional<std::string> pick_model_file() {
    return pick_open("Ouvrir un modele 3D", "Modeles 3D (*.bbmodel *.json)",
                     "*.bbmodel *.json", "");
}

std::optional<std::string> pick_zip_save(const std::string& defaultName) {
    auto r = pick_save("Exporter l'instance", defaultName, "Archive ZIP (*.zip)",
                       "*.zip");
    if (r) *r = ensure_ext(*r, "zip");
    return r;
}

std::optional<std::string> pick_folder(const char* title) {
    return pick_dir(title ? title : "");
}

std::optional<std::string> pick_file_open(const std::string& title,
                                          const std::string& filterName,
                                          const std::string& filterPat,
                                          const std::string& startFile) {
    return pick_open(title, filterName, filterPat, startFile);
}

std::vector<std::string> pick_files_open(const std::string& title,
                                         const std::string& filterName,
                                         const std::string& filterPat,
                                         const std::string& startDir) {
    return pick_multi(title, filterName, filterPat, startDir);
}

std::optional<std::string> pick_file_save(const std::string& title,
                                          const std::string& filterName,
                                          const std::string& filterPat,
                                          const std::string& defaultName,
                                          const std::string& defExt) {
    auto r = pick_save(title, defaultName, filterName, filterPat);
    if (r) *r = ensure_ext(*r, defExt);
    return r;
}

#endif

} // namespace tl::ui

#include "plugins.hpp"

#include "datastore.hpp"
#include "util_hash.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#define NOMINMAX
#include <Windows.h>
#else
#include "proc.hpp"
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::plugins {

namespace {

// Empreinte du manifeste. MD5 suffit : il s'agit de detecter qu'un
// fichier a CHANGE depuis l'autorisation, pas de resister a quelqu'un qui
// fabriquerait une collision — celui qui peut reecrire plugin.json peut
// aussi bien reecrire la configuration qui porte l'autorisation.
std::string fingerprint(const std::string& text) {
    auto d = md5_digest(text);
    if (!d) return {};
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char c : *d) {
        out.push_back(hex[c >> 4]);
        out.push_back(hex[c & 0xF]);
    }
    return out;
}

// Entree de la liste des autorisations : « id:empreinte ».
std::string allow_key(const Plugin& p) { return p.id + ":" + p.manifestHash; }

#ifdef _WIN32
std::wstring to_w(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                      static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}

// Echappement d'un argument pour CommandLineToArgvW, qui est la grammaire
// que lira le programme appele. Les regles sont contre-intuitives : les
// antislashs ne comptent que devant un guillemet.
std::string win_quote(const std::string& a) {
    if (!a.empty() &&
        a.find_first_of(" \t\n\v\"") == std::string::npos)
        return a;
    std::string out = "\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < a.size() && a[i] == '\\') {
            ++i;
            ++slashes;
        }
        if (i == a.size()) {
            out.append(slashes * 2, '\\');
            break;
        }
        if (a[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(slashes, '\\');
            out.push_back(a[i]);
        }
    }
    out.push_back('"');
    return out;
}
#endif

} // namespace

fs::path dir() {
    const fs::path d = DataStore::dir() / "plugins";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

Plugin parse_manifest(const std::string& text, const std::string& id) {
    Plugin p;
    p.id = id;
    // Nom de repli posé AVANT toute analyse : un manifeste illisible
    // sortait d'ici sans nom, et l'interface affichait une erreur sans
    // dire de quel plugin elle parlait — exactement le cas où le nom est
    // le plus nécessaire.
    p.name = id;
    p.manifestHash = fingerprint(text);
    json j;
    try {
        j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& ex) {
        p.error = std::string("plugin.json illisible : ") + ex.what();
        return p;
    }
    if (!j.is_object()) {
        p.error = "plugin.json : objet attendu.";
        return p;
    }
    p.name = j.value("name", id);
    p.version = j.value("version", std::string{});
    p.description = j.value("description", std::string{});
    p.author = j.value("author", std::string{});

    auto acts = j.find("actions");
    if (acts == j.end() || !acts->is_array() || acts->empty()) {
        p.error = "plugin.json : aucune action déclarée.";
        return p;
    }
    for (const auto& a : *acts) {
        if (!a.is_object()) continue;
        Action x;
        x.label = a.value("label", std::string{});
        x.program = a.value("run", std::string{});
        x.workDir = a.value("workDir", std::string{});
        const std::string w = a.value("where", std::string("tools"));
        x.where = w == "instance" ? Where::Instance : Where::Tools;
        if (auto ar = a.find("args"); ar != a.end() && ar->is_array())
            for (const auto& v : *ar)
                if (v.is_string()) x.args.push_back(v.get<std::string>());
        if (x.label.empty() || x.program.empty()) continue;
        // Un « programme » qui contient un séparateur de commandes n'est
        // pas un programme : c'est quelqu'un qui espère un shell. On le
        // refuse plutôt que de l'exécuter par un chemin détourné.
        if (x.program.find_first_of("&|<>;\n") != std::string::npos) {
            p.error = "plugin.json : « run » doit être un programme, pas une "
                      "ligne de commande (« " + x.program + " »).";
            return p;
        }
        p.actions.push_back(std::move(x));
    }
    if (p.actions.empty() && p.error.empty())
        p.error = "plugin.json : aucune action exploitable (il faut « label » "
                  "et « run »).";
    return p;
}

std::vector<Plugin> list() {
    std::vector<Plugin> out;
    const fs::path root = dir();
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return out;
    const auto& allowed = DataStore::settings.enabledPlugins;

    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        const fs::path manifest = e.path() / "plugin.json";
        if (!fs::is_regular_file(manifest, ec)) continue;
        std::ifstream in(manifest, std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
        Plugin p = parse_manifest(text, e.path().filename().string());
        p.dir = e.path();
        p.enabled = std::find(allowed.begin(), allowed.end(), allow_key(p)) !=
                    allowed.end();
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(),
              [](const Plugin& a, const Plugin& b) { return a.name < b.name; });
    return out;
}

void set_enabled(const Plugin& p, bool on) {
    auto& v = DataStore::settings.enabledPlugins;
    const std::string key = allow_key(p);
    // On retire d'abord TOUTES les entrées de ce plugin, quelle que soit
    // leur empreinte : sans cela, désactiver puis réactiver après une
    // modification laisserait l'ancienne autorisation traîner, et une
    // restauration de l'ancien manifeste le rendrait actif sans un clic.
    const std::string prefix = p.id + ":";
    v.erase(std::remove_if(v.begin(), v.end(),
                           [&](const std::string& s) {
                               return s.rfind(prefix, 0) == 0;
                           }),
            v.end());
    if (on) v.push_back(key);
    DataStore::save();
}

std::string expand(const std::string& s, const Context& ctx) {
    struct Sub {
        const char* key;
        const std::string* val;
    };
    const Sub subs[] = {
        {"{instanceId}", &ctx.instanceId},
        {"{instanceName}", &ctx.instanceName},
        {"{instanceDir}", &ctx.instanceDir},
        {"{gameVersion}", &ctx.gameVersion},
        {"{dataDir}", &ctx.dataDir},
        {"{pluginDir}", &ctx.pluginDir},
    };
    std::string out = s;
    for (const auto& sub : subs) {
        // Valeur absente : on laisse le substituable en clair. Le
        // remplacer par du vide transformerait « {instanceDir}/mods » en
        // « /mods », qui désigne un dossier bien réel et pas celui-là.
        if (sub.val->empty()) continue;
        const std::string key = sub.key;
        size_t pos = 0;
        while ((pos = out.find(key, pos)) != std::string::npos) {
            out.replace(pos, key.size(), *sub.val);
            pos += sub.val->size();
        }
    }
    return out;
}

std::string preview(const Action& a, const Context& ctx) {
    auto quote = [](const std::string& s) {
        return s.find_first_of(" \t\"") == std::string::npos ? s
                                                             : "\"" + s + "\"";
    };
    std::string out = quote(expand(a.program, ctx));
    for (const auto& arg : a.args) out += " " + quote(expand(arg, ctx));
    return out;
}

bool run(const Plugin& p, const Action& a, const Context& ctx,
         std::string* errOut) {
    auto fail = [&](const std::string& m) {
        if (errOut) *errOut = m;
        return false;
    };
    // Le contrôle est refait ici, et pas seulement dans l'interface : un
    // appel venu d'ailleurs (API locale, test) doit buter sur la même
    // règle.
    if (!p.enabled)
        return fail("Ce plugin n'est pas autorisé. Autorisez-le dans "
                    "Paramètres > Plugins après avoir lu sa commande.");
    if (a.program.empty()) return fail("Action sans programme.");

    const std::string program = expand(a.program, ctx);
    std::vector<std::string> args;
    args.reserve(a.args.size());
    for (const auto& x : a.args) args.push_back(expand(x, ctx));
    const std::string work =
        a.workDir.empty() ? p.dir.string() : expand(a.workDir, ctx);

#ifdef _WIN32
    // Pas de shell : CreateProcessW reçoit le programme et une ligne dont
    // chaque argument est échappé. `&&`, `|` ou `>` dans un argument
    // restent donc du texte.
    std::string cmd = win_quote(program);
    for (const auto& x : args) cmd += " " + win_quote(x);
    std::wstring wcmd = to_w(cmd);
    wcmd.push_back(L'\0');
    const std::wstring wwork = to_w(work);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(
        nullptr, wcmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
        nullptr, wwork.empty() ? nullptr : wwork.c_str(), &si, &pi);
    if (!ok)
        return fail("Lancement impossible (code " +
                    std::to_string(GetLastError()) + ") : " + program);
    // On n'attend pas : un plugin qui ouvre une fenêtre ne doit pas geler
    // le launcher. Les poignées sont relâchées tout de suite, le système
    // nettoiera le processus à sa sortie.
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    std::vector<std::string> argv;
    argv.push_back(program); // argv[0] = chemin, cf. proc.hpp
    for (const auto& x : args) argv.push_back(x);
    auto child = proc::spawn(program, argv, work, /*mergeStderr=*/true);
    if (!child) return fail("Lancement impossible : " + program);
    // Même raison que sous Windows : on ne retient pas le launcher. Les
    // descripteurs rendus par spawn sont refermés ici.
    proc::close_fd(child->outFd);
    proc::close_fd(child->errFd);
    return true;
#endif
}

} // namespace tl::plugins

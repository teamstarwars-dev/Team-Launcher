#include "marketplace.hpp"

#include "datastore.hpp"
#include "http_win.hpp"
#include "plugin_telemetry.hpp"
#include "plugins.hpp"

#include "miniz.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <iterator>
#include <random>
#include <regex>
#include <set>
#include <sstream>
#include <thread>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::market {
namespace {

// Mêmes bornes que le serveur (marketplace/app/config.py).
constexpr std::size_t kMaxDownload = 80ull * 1024 * 1024; // 64 Mio + marge
constexpr std::uint64_t kMaxUnpacked = 256ull * 1024 * 1024;
constexpr std::size_t kMaxFiles = 4096;
constexpr std::size_t kMaxCatalog = 4 * 1024 * 1024;
constexpr std::size_t kMaxEntries = 2000;

// Mêmes règles que le serveur (marketplace/app/packages.py).
const std::regex& slug_re() {
    static const std::regex r("^[a-z0-9_-][a-z0-9._-]{0,63}$");
    return r;
}
const std::regex& version_re() {
    static const std::regex r("^[0-9A-Za-z][0-9A-Za-z.+_-]{0,31}$");
    return r;
}

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}
fs::path from_u8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

bool valid_utf8(const std::string& s) {
    try {
        (void)json(s).dump();
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

std::string rand_suffix() {
    std::random_device rd;
    std::ostringstream o;
    o << std::hex << rd() << rd();
    return o.str();
}

// Nom d'entrée d'archive acceptable ? Même grammaire que le serveur.
bool safe_entry_name(const std::string& name) {
    if (name.empty() || name.size() > 512 || !valid_utf8(name)) return false;
    if (name.find('\\') != std::string::npos || name.front() == '/') return false;
    if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) && name[1] == ':')
        return false;
    for (const char c : name)
        if (static_cast<unsigned char>(c) < 0x20) return false;
    std::string trimmed = name;
    if (trimmed.back() == '/') trimmed.pop_back();
    std::size_t b = 0;
    while (b <= trimmed.size()) {
        const std::size_t e = std::min(trimmed.find('/', b), trimmed.size());
        const std::string part = trimmed.substr(b, e - b);
        if (part.empty() || part == "." || part == "..") return false;
        b = e + 1;
    }
    return true;
}

// Sous Windows, l'antivirus ouvre un exécutable fraîchement écrit pour
// l'analyser : pendant ce temps, renommer ou supprimer son dossier échoue.
// On réessaie pendant quelques secondes plutôt que d'échouer d'emblée.
template <class F> bool retry(F&& op) {
    for (int i = 0; i < 40; ++i) {
        if (op()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(i < 10 ? 50 : 150));
    }
    return false;
}

bool rename_retry(const fs::path& from, const fs::path& to, std::error_code& ec) {
    return retry([&] {
        ec.clear();
        fs::rename(from, to, ec);
        return !ec;
    });
}

void remove_quiet(const fs::path& p) {
    retry([&] {
        std::error_code ec;
        fs::remove_all(p, ec);
        return !fs::exists(p, ec);
    });
}

Result fail(const std::string& m) { return Result{false, m}; }

} // namespace

std::optional<std::string> normalize_base(const std::string& url, std::string* err) {
    auto bad = [&](const char* why) -> std::optional<std::string> {
        if (err) *err = why;
        return std::nullopt;
    };
    if (url.empty()) return bad("adresse de la marketplace non configurée");
    if (url.size() > 300) return bad("adresse trop longue");
    for (const char c : url)
        if (static_cast<unsigned char>(c) <= 0x20 || c == '@' || c == '?' || c == '#' ||
            c == '\\')
            return bad("adresse invalide (espaces, identifiants, requête ou fragment interdits)");
    std::string s = url;
    while (!s.empty() && s.back() == '/') s.pop_back();
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::size_t hostStart;
    if (lower.rfind("https://", 0) == 0) {
        hostStart = 8;
    } else if (lower.rfind("http://", 0) == 0) {
        hostStart = 7;
        std::string host = lower.substr(7, lower.find('/', 7) - 7);
        if (const auto colon = host.rfind(':'); colon != std::string::npos && host.back() != ']')
            host.resize(colon);
        // En clair, uniquement vers soi-même : un serveur de test local.
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
            return bad("HTTPS obligatoire (http:// seulement vers 127.0.0.1 ou localhost)");
    } else {
        return bad("l'adresse doit commencer par https://");
    }
    if (s.size() <= hostStart || s[hostStart] == '/' || s[hostStart] == ':')
        return bad("adresse sans hôte");
    return s;
}

std::vector<Entry> parse_catalog(const std::string& text, std::string* err) {
    std::vector<Entry> out;
    if (text.size() > kMaxCatalog) {
        if (err) *err = "catalogue démesuré";
        return out;
    }
    json doc = json::parse(text, nullptr, false);
    if (!doc.is_array()) {
        if (err) *err = "catalogue illisible";
        return out;
    }
    std::set<std::pair<int, std::string>> seen;
    for (const auto& j : doc) {
        if (out.size() >= kMaxEntries) break;
        if (!j.is_object()) continue;
        auto str = [&](const char* k) {
            const auto it = j.find(k);
            return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        Entry e;
        const std::string kind = str("kind");
        if (kind == "plugin") e.kind = signing::Kind::Plugin;
        else if (kind == "mod") e.kind = signing::Kind::Mod;
        else continue;
        e.slug = str("slug");
        e.version = str("version");
        if (!std::regex_match(e.slug, slug_re()) || !std::regex_match(e.version, version_re()))
            continue;
        if (!seen.insert({static_cast<int>(e.kind), e.slug}).second) continue;
        e.name = str("name").substr(0, 128);
        e.description = str("description").substr(0, 500);
        if (e.name.empty()) e.name = e.slug;
        if (const auto p = j.find("permissions"); p != j.end() && p->is_array())
            for (const auto& x : *p)
                if (x.is_string() && e.permissions.size() < 32)
                    e.permissions.push_back(x.get<std::string>().substr(0, 64));
        out.push_back(std::move(e));
    }
    return out;
}

bool extract_package(const std::string& zipBytes, const fs::path& dest, std::string* err) {
    auto bad = [&](const std::string& why) {
        if (err) *err = why;
        remove_quiet(dest);
        return false;
    };
    std::error_code ec;
    if (fs::exists(dest, ec) && !fs::is_empty(dest, ec)) {
        if (err) *err = "dossier de destination non vide";
        return false;
    }
    if (zipBytes.size() > kMaxDownload) return bad("archive trop volumineuse");

    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0))
        return bad("archive ZIP illisible");
    struct Closer {
        mz_zip_archive* z;
        ~Closer() { mz_zip_reader_end(z); }
    } closer{&zip};

    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    if (count == 0) return bad("archive vide");
    if (count > kMaxFiles) return bad("trop de fichiers dans l'archive");

    // Premier passage : rien n'est écrit tant que TOUTE l'archive n'a pas
    // été jugée acceptable sur ses métadonnées.
    std::uint64_t declared = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return bad("entrée illisible");
        const std::string name = st.m_filename;
        if (!safe_entry_name(name)) return bad("chemin refusé dans l'archive : " + name);
        if (st.m_is_encrypted) return bad("entrée chiffrée refusée : " + name);
        if (!st.m_is_supported) return bad("compression non prise en charge : " + name);
        // Lien symbolique stocké dans les attributs Unix de l'entrée.
        if (((st.m_external_attr >> 16) & 0170000) == 0120000)
            return bad("lien symbolique refusé : " + name);
        declared += st.m_uncomp_size;
        if (declared > kMaxUnpacked) return bad("archive trop volumineuse une fois décompressée");
    }

    fs::create_directories(dest, ec);
    if (ec) return bad("impossible de créer le dossier de travail");
    std::uint64_t written = 0;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        mz_zip_reader_file_stat(&zip, i, &st);
        const std::string name = st.m_filename;
        const fs::path out = dest / from_u8(name);
        if (mz_zip_reader_is_file_a_directory(&zip, i)) {
            fs::create_directories(out, ec);
            continue;
        }
        fs::create_directories(out.parent_path(), ec);
        // Fichier déjà là : deux entrées qui ne diffèrent que par la casse
        // (ou un doublon). L'une écraserait l'autre : refus.
        if (fs::exists(out, ec)) return bad("doublon dans l'archive : " + name);
        // miniz vérifie que la taille réelle égale la taille déclarée, et
        // le CRC : une entrée qui ment sur sa taille échoue ici.
        std::size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) return bad("entrée corrompue : " + name);
        written += size;
        bool ok = written <= kMaxUnpacked;
        if (ok) {
            std::ofstream f(out, std::ios::binary | std::ios::trunc);
            f.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            ok = static_cast<bool>(f);
        }
        mz_free(data);
        if (!ok) return bad("écriture impossible : " + name);
    }
    return true;
}

Result install_from_zip(const std::string& zipBytes, const Entry& e, const fs::path& targetDir,
                        const fs::path& workDir, const signing::Roots& roots,
                        const signing::Revocations* revs, std::int64_t now) {
    if (!std::regex_match(e.slug, slug_re()) || !std::regex_match(e.version, version_re()))
        return fail("identifiant ou version invalide");
    std::error_code ec;
    fs::create_directories(workDir, ec);
    fs::create_directories(targetDir, ec);
    const fs::path staging = workDir / ("new-" + e.slug + "-" + rand_suffix());
    std::string err;
    if (!extract_package(zipBytes, staging, &err)) return fail("Archive refusée : " + err);

    const auto v = signing::verify_release_dir(staging, e.kind, e.slug, e.version, roots, revs, now);
    if (v.status != signing::Status::Valid) {
        remove_quiet(staging);
        switch (v.status) {
        case signing::Status::Unsigned:
            return fail(roots.keys.empty()
                            ? "Ce launcher ne contient encore aucune clé racine : impossible de "
                              "vérifier une signature, donc rien n'est installé."
                            : "Archive non signée : refusée (" + v.detail + ").");
        case signing::Status::Revoked: return fail("Version révoquée : " + v.detail + ".");
        default: return fail("Signature refusée : " + v.detail + ".");
        }
    }

    if (e.kind == signing::Kind::Plugin) {
        // La signature couvre plugin.json ; on vérifie en plus qu'il est
        // compris du launcher et qu'il dit la même version.
        // Fichier FERMÉ avant le renommage : sous Windows, un fichier ouvert
        // interdit de renommer le dossier qui le contient.
        std::string text;
        {
            std::ifstream in(staging / "plugin.json", std::ios::binary);
            text.assign(std::istreambuf_iterator<char>(in), {});
        }
        const plugins::Plugin p = plugins::parse_manifest(text, e.slug);
        if (!p.error.empty() || p.version != e.version) {
            remove_quiet(staging);
            return fail("Manifeste refusé : " + (p.error.empty() ? "version discordante" : p.error));
        }
        const fs::path target = targetDir / e.slug;
        const fs::path old = workDir / ("old-" + e.slug + "-" + rand_suffix());
        const bool replacing = fs::exists(target, ec);
        if (replacing) {
            if (!rename_retry(target, old, ec)) {
                remove_quiet(staging);
                return fail("Impossible de remplacer la version installée : son service "
                            "tourne-t-il encore ? Arrêtez-le, puis réessayez.");
            }
        }
        if (!rename_retry(staging, target, ec)) {
            const std::string why = ec.message();
            if (replacing) rename_retry(old, target, ec); // on remet l'ancienne
            remove_quiet(staging);
            return fail("Installation impossible (renommage refusé : " + why + ").");
        }
        remove_quiet(old);
        return Result{true, p.name + " " + e.version + " installé. Lisez ses permissions puis "
                                                       "autorisez-le dans la liste des plugins."};
    }

    // Mod : uniquement des .jar à la racine, jamais d'écrasement.
    std::vector<fs::path> jars;
    for (const auto& ent : fs::directory_iterator(staging, ec)) {
        const std::string name = u8(ent.path().filename());
        if (name == "signature.json") continue;
        std::string low = name;
        std::transform(low.begin(), low.end(), low.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!ent.is_regular_file(ec) || low.size() < 5 || low.substr(low.size() - 4) != ".jar") {
            remove_quiet(staging);
            return fail("Un paquet de mod ne doit contenir que des fichiers .jar (trouvé : " +
                        name + ").");
        }
        if (fs::exists(targetDir / ent.path().filename(), ec)) {
            remove_quiet(staging);
            return fail(name + " est déjà présent dans les mods de l'instance.");
        }
        jars.push_back(ent.path());
    }
    if (jars.empty()) {
        remove_quiet(staging);
        return fail("Paquet de mod vide.");
    }
    std::vector<fs::path> done;
    for (const auto& j : jars) {
        const fs::path dst = targetDir / j.filename();
        const fs::path part = targetDir / (j.filename().u8string() + u8".tl-part");
        fs::copy_file(j, part, fs::copy_options::overwrite_existing, ec);
        if (!ec) rename_retry(part, dst, ec);
        if (ec) {
            remove_quiet(part);
            for (const auto& d : done) remove_quiet(d);
            remove_quiet(staging);
            return fail("Copie impossible vers le dossier mods.");
        }
        done.push_back(dst);
    }
    remove_quiet(staging);
    return Result{true, e.name + " " + e.version + " ajouté aux mods de l'instance."};
}

// --- Réseau -------------------------------------------------------------------

namespace {

std::optional<std::string> base_or_err(std::string* err) {
    auto b = normalize_base(DataStore::settings.marketplaceUrl, err);
    if (b) http::allow_host(*b); // adresse modifiée après le démarrage
    return b;
}

std::optional<http::Response> get(const std::string& url, std::string* err,
                                  const std::atomic<bool>* cancel) {
    auto r = http::get_response(url, {}, cancel);
    if (!r) {
        if (err) *err = "serveur injoignable";
        return std::nullopt;
    }
    return r;
}

} // namespace

std::optional<std::vector<Entry>> fetch_catalog(std::string* err,
                                                const std::atomic<bool>* cancel) {
    const auto base = base_or_err(err);
    if (!base) return std::nullopt;
    const auto r = get(*base + "/v1/public/catalog", err, cancel);
    if (!r) return std::nullopt;
    if (r->status != 200) {
        if (err) *err = "le serveur a répondu " + std::to_string(r->status);
        return std::nullopt;
    }
    std::string perr;
    auto list = parse_catalog(r->body, &perr);
    if (!perr.empty()) {
        if (err) *err = perr;
        return std::nullopt;
    }
    return list;
}

Result install(const Entry& e, const fs::path& modsDir, const std::atomic<bool>* cancel) {
    std::string err;
    const auto base = base_or_err(&err);
    if (!base) return fail(err);
    const char* kind = e.kind == signing::Kind::Plugin ? "plugin" : "mod";
    if (!std::regex_match(e.slug, slug_re()) || !std::regex_match(e.version, version_re()))
        return fail("identifiant ou version invalide");
    const auto r = get(*base + "/v1/public/download/" + kind + "/" + e.slug + "/" + e.version,
                       &err, cancel);
    if (!r) return fail("Téléchargement impossible : " + err + ".");
    if (r->status != 200)
        return fail("Téléchargement refusé (HTTP " + std::to_string(r->status) + ").");
    if (r->body.size() > kMaxDownload) return fail("Archive trop volumineuse.");

    const fs::path pluginsDir = plugins::dir();
    const bool isPlugin = e.kind == signing::Kind::Plugin;
    const fs::path target = isPlugin ? pluginsDir : modsDir;
    if (target.empty()) return fail("Aucune instance choisie pour ce mod.");
    // Dossier de travail À CÔTÉ de la destination : même volume, donc
    // l'installation finale est un renommage, pas une copie interrompable.
    const fs::path work = isPlugin ? pluginsDir.parent_path() / "plugins-staging"
                                   : target.parent_path() / ".tl-staging";
    const signing::Revocations revs = signing::current_revocations();
    Result res = install_from_zip(r->body, e, target, work, signing::builtin_roots(), &revs,
                                  static_cast<std::int64_t>(std::time(nullptr)));
    std::error_code ec;
    fs::remove(work, ec); // vide après une installation, réussie ou non
    return res;
}

bool refresh_revocations(std::string* err, const std::atomic<bool>* cancel) {
    const auto base = base_or_err(err);
    if (!base) return false;
    const auto r = get(*base + "/v1/public/revocations", err, cancel);
    if (!r) return false;
    if (r->status == 404) return true; // aucune liste publiée
    if (r->status != 200) {
        if (err) *err = "le serveur a répondu " + std::to_string(r->status);
        return false;
    }
    return signing::install_revocations(r->body, static_cast<std::int64_t>(std::time(nullptr)),
                                        err);
}

bool send_telemetry(std::string* err, const std::atomic<bool>* cancel) {
    if (!DataStore::settings.pluginTelemetry) {
        plugtele::take(); // désactivée entre-temps : on jette, on n'envoie pas
        return true;
    }
    const auto base = base_or_err(err);
    if (!base) return false;
    const plugtele::Counts c = plugtele::take();
    if (c.empty()) return true;
#ifdef _WIN32
    const char* os = "windows";
#else
    const char* os = "linux";
#endif
#ifndef TL_VERSION_STRING
#define TL_VERSION_STRING "dev"
#endif
    const auto r = http::post_string(*base + "/v1/public/telemetry",
                                     plugtele::payload(c, TL_VERSION_STRING, os),
                                     "application/json", {}, cancel);
    if (r && (r->status == 204 || r->status == 200)) return true;
    // Réseau, débit ou serveur en panne : on garde pour plus tard. Un refus
    // de validation (4xx) ne se corrigera pas en réessayant : on jette.
    if (!r || r->status == 429 || r->status >= 500) plugtele::restore(c);
    if (err) *err = r ? "le serveur a répondu " + std::to_string(r->status) : "serveur injoignable";
    return false;
}

} // namespace tl::market

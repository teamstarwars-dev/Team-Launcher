#include "modupdate.hpp"

#include "curseforge.hpp" // cf::sanitize (nom de fichier sur)
#include "http_win.hpp"
#include "modmeta.hpp"
#include "modrinth.hpp"
#include "util_hash.hpp"

#include <algorithm>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace tl::modupdate {

namespace {

// Nom de chargeur tel que Modrinth l'attend : en minuscules.
std::string loader_slug(const std::string& s) {
    std::string t;
    for (char c : s) t.push_back(static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(c))));
    if (t == "vanilla" || t.empty()) return {};
    return t;
}

} // namespace

Result check(const fs::path& modsDir, const std::string& loader,
             const std::string& mcVersion, const std::atomic<bool>* cancel,
             Progress progress) {
    Result res;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return res;

    // 1. Les jars actifs, et leur SHA-1.
    struct Local {
        fs::path file;
        std::string sha1;
        std::string name;
        std::string version;
    };
    std::vector<Local> locals;
    for (const auto& m : modmeta::read_dir(modsDir)) {
        if (m.disabled) continue;
        Local l;
        l.file = modsDir / m.file;
        l.name = m.name.empty() ? m.file : m.name;
        l.version = m.version;
        if (auto h = sha1_hex(l.file)) l.sha1 = *h;
        if (l.sha1.empty()) continue; // illisible : rien a demander
        // L'API rend les empreintes en minuscules ; on s'aligne pour
        // pouvoir comparer sans retraiter.
        for (char& c : l.sha1)
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        locals.push_back(std::move(l));
    }
    res.checked = static_cast<int>(locals.size());
    if (locals.empty()) return res;
    if (cancel && cancel->load()) return res;

    // 2. Une seule requete groupee pour retrouver les projets.
    std::vector<std::string> hashes;
    hashes.reserve(locals.size());
    for (const auto& l : locals) hashes.push_back(l.sha1);
    const auto matches = mr::version_files(hashes, cancel);
    if (matches.empty()) {
        res.unknown = res.checked;
        res.error =
            "Aucun mod reconnu sur Modrinth. Si la liste n'est pas vide, "
            "c'est probablement que le réseau n'a pas répondu.";
        return res;
    }

    // 3. Pour chaque projet reconnu, la liste de ses versions compatibles.
    // Une requete par projet : Modrinth n'offre pas d'appel groupe pour
    // cela. On garde un cache local, plusieurs jars pouvant appartenir au
    // meme projet (rare, mais le cas des bibliotheques partagees existe).
    const std::string ldr = loader_slug(loader);
    std::map<std::string, std::vector<mr::Version>> cacheByProject;
    int done = 0;
    for (const auto& l : locals) {
        if (cancel && cancel->load()) break;
        ++done;
        if (progress) progress(done, static_cast<int>(locals.size()));

        auto it = matches.find(l.sha1);
        if (it == matches.end() || it->second.projectId.empty()) {
            ++res.unknown;
            continue;
        }
        const std::string& pid = it->second.projectId;
        // Version installee : celle que l'API associe a l'empreinte fait
        // foi, car c'est la meme echelle que celles qu'on va comparer. Le
        // manifeste du jar ne sert que de repli.
        const std::string cur = !it->second.versionNumber.empty()
                                    ? it->second.versionNumber
                                    : l.version;

        auto cached = cacheByProject.find(pid);
        if (cached == cacheByProject.end())
            cached = cacheByProject
                         .emplace(pid, mr::project_versions(pid, ldr, mcVersion,
                                                            cancel))
                         .first;
        const auto& versions = cached->second;
        if (versions.empty()) {
            // Projet connu mais aucune version pour ce chargeur / cette
            // version du jeu : ce n'est pas « a jour », c'est « rien a
            // proposer ». Le compter comme a jour serait mentir.
            ++res.unknown;
            continue;
        }

        // Modrinth rend la plus recente d'abord. On ne se fie pas qu'a cet
        // ordre : on compare les numeros, et on ne propose que du plus
        // recent — une API qui renverrait l'ordre inverse ne doit pas
        // faire proposer un retour en arriere.
        const mr::Version* best = nullptr;
        for (const auto& v : versions) {
            if (v.url.empty()) continue;
            if (!cur.empty() && modmeta::compare_versions(v.number, cur) <= 0)
                continue;
            if (!best || modmeta::compare_versions(v.number, best->number) > 0)
                best = &v;
        }
        if (!best) {
            ++res.upToDate;
            continue;
        }
        Update u;
        u.file = l.file;
        u.displayName = l.name;
        u.projectId = pid;
        u.currentVersion = cur;
        u.newVersion = best->number;
        u.changelog = best->changelog;
        u.downloadUrl = best->url;
        u.newFilename = best->filename.empty() ? (pid + "-" + best->number + ".jar")
                                               : best->filename;
        res.updates.push_back(std::move(u));
    }
    return res;
}

bool apply(const Update& u, const std::atomic<bool>* cancel,
           std::string* errOut) {
    auto fail = [&](const std::string& m) {
        if (errOut) *errOut = m;
        return false;
    };
    if (u.downloadUrl.empty()) return fail("Aucune URL de téléchargement.");
    std::error_code ec;
    const fs::path dir = u.file.parent_path();
    const fs::path dest = dir / cf::sanitize(u.newFilename);

    // Le nouveau fichier peut porter le meme nom que l'ancien (auteur qui
    // ne versionne pas ses fichiers). Ecrire d'abord a cote sous un nom
    // temporaire evite d'ecraser le mod qui marche avant d'etre sur que le
    // telechargement a abouti.
    const fs::path tmp = dir / (dest.filename().string() + ".tl-part");
    fs::remove(tmp, ec);
    if (!http::get_to_file(u.downloadUrl, tmp, nullptr, cancel)) {
        fs::remove(tmp, ec);
        return fail("Téléchargement impossible (réseau ou fichier retiré).");
    }
    if (!fs::is_regular_file(tmp, ec) || fs::file_size(tmp, ec) == 0) {
        fs::remove(tmp, ec);
        return fail("Fichier téléchargé vide.");
    }

    // L'ancien part MAINTENANT, une fois le nouveau sur le disque. Si
    // l'ancien et le nouveau portent le meme nom, l'ordre est le seul
    // moyen de ne pas se retrouver sans mod du tout.
    if (fs::exists(u.file, ec)) {
        fs::remove(u.file, ec);
        if (ec) {
            fs::remove(tmp, ec);
            return fail("Impossible de retirer l'ancienne version (fichier "
                        "verrouillé ? le jeu tourne-t-il ?).");
        }
    }
    fs::remove(dest, ec);
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return fail("Impossible de mettre le nouveau fichier en place.");
    }
    return true;
}

} // namespace tl::modupdate

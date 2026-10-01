#include "modcheck.hpp"

#include <algorithm>
#include <map>
#include <set>

namespace tl::modcheck {

namespace {

// Identifiants qui ne designent pas un mod installe : ils sont fournis par
// le jeu, le chargeur ou la machine virtuelle. Les chercher dans le
// dossier mods produirait des « dependance manquante » systematiques.
bool is_platform_id(const std::string& id) {
    static const std::set<std::string> kPlatform = {
        "minecraft", "java", "forge", "neoforge", "fabricloader",
        "fabric-loader", "quilt_loader", "quilt_base", "mcp",
    };
    return kPlatform.count(id) != 0;
}

void add(Report& r, Severity sev, const std::string& file,
         const std::string& title, const std::string& detail) {
    r.issues.push_back(Issue{sev, file, title, detail});
    switch (sev) {
    case Severity::Error: ++r.errors; break;
    case Severity::Warning: ++r.warnings; break;
    default: ++r.infos; break;
    }
}

} // namespace

const char* severity_label(Severity s) {
    switch (s) {
    case Severity::Error: return "Bloquant";
    case Severity::Warning: return "Avertissement";
    default: return "Remarque";
    }
}

Report analyse(const std::vector<modmeta::Mod>& mods, const std::string& loader,
               const std::string& mcVersion) {
    Report r;
    const modmeta::Loader want = modmeta::loader_from_string(loader);

    // Index des mods actifs, par modId. Un meme id peut apparaitre deux
    // fois : c'est precisement un des conflits qu'on cherche.
    std::map<std::string, std::vector<const modmeta::Mod*>> byId;
    std::vector<const modmeta::Mod*> active;
    for (const auto& m : mods) {
        if (m.disabled) {
            ++r.skipped;
            continue;
        }
        active.push_back(&m);
        if (!m.id.empty()) byId[m.id].push_back(&m);
    }
    r.analysed = static_cast<int>(active.size());

    // --- Cas ou l'instance n'a pas de chargeur -----------------------------
    // Sans chargeur, Minecraft ignore purement et simplement le dossier
    // mods/. Le dire une fois vaut mieux que de signaler quarante mods
    // « pour le mauvais chargeur ».
    if (want == modmeta::Loader::Unknown && !active.empty()) {
        add(r, Severity::Error, "",
            "Aucun chargeur de mods sur cette instance",
            "L'instance est en Vanilla : Minecraft n'ouvrira même pas le "
            "dossier mods/, et les " + std::to_string(active.size()) +
                " mods présents resteront sans effet. Choisissez Fabric, "
                "Forge, NeoForge ou Quilt dans les paramètres de l'instance.");
        return r;
    }

    for (const auto* mp : active) {
        const auto& m = *mp;

        // --- Archive illisible ---------------------------------------------
        if (!m.readError.empty()) {
            add(r, Severity::Warning, m.file, "Métadonnées illisibles",
                m.readError +
                    ". Le fichier n'est peut-être pas un mod, ou le "
                    "téléchargement est incomplet.");
            continue;
        }

        // --- Mauvais chargeur ----------------------------------------------
        // Quilt charge les mods Fabric : ce n'est donc pas un conflit.
        // L'inverse est faux, et NeoForge n'accepte plus les mods Forge
        // depuis 1.20.5 — on le signale sans l'affirmer comme bloquant,
        // beaucoup de mods restant compatibles en pratique.
        if (m.loader != modmeta::Loader::Unknown && m.loader != want) {
            const bool quiltTakesFabric = want == modmeta::Loader::Quilt &&
                                          m.loader == modmeta::Loader::Fabric;
            const bool forgeFamily =
                (want == modmeta::Loader::NeoForge &&
                 m.loader == modmeta::Loader::Forge) ||
                (want == modmeta::Loader::Forge &&
                 m.loader == modmeta::Loader::NeoForge);
            if (!quiltTakesFabric) {
                add(r, forgeFamily ? Severity::Warning : Severity::Error, m.file,
                    std::string("Prévu pour ") + modmeta::loader_name(m.loader),
                    std::string("L'instance utilise ") +
                        modmeta::loader_name(want) + ". " +
                        (forgeFamily
                             ? "Forge et NeoForge sont proches mais ont "
                               "divergé : ce mod peut fonctionner comme il "
                               "peut refuser de se charger."
                             : "Ce mod ne se chargera pas. Téléchargez la "
                               "version " +
                                   std::string(modmeta::loader_name(want)) +
                                   " du mod."));
                continue;
            }
        }

        // --- Version de Minecraft -------------------------------------------
        if (!mcVersion.empty() && !m.mcRange.empty() &&
            !modmeta::version_matches(mcVersion, m.mcRange)) {
            add(r, Severity::Error, m.file, "Version de Minecraft incompatible",
                "Le mod déclare fonctionner avec « " + m.mcRange +
                    " », or l'instance est en " + mcVersion + ".");
        }

        // --- Dependances -----------------------------------------------------
        for (const auto& d : m.deps) {
            if (d.id.empty() || is_platform_id(d.id)) continue;
            auto it = byId.find(d.id);
            if (it == byId.end()) {
                if (d.mandatory) {
                    add(r, Severity::Error, m.file,
                        "Dépendance manquante : " + d.id,
                        "Ce mod a besoin de « " + d.id + " »" +
                            (d.range.empty() || d.range == "*"
                                 ? ""
                                 : " (version " + d.range + ")") +
                            ", qui n'est pas installé — ou est désactivé.");
                } else {
                    // Facultatif et absent : c'est une suggestion, pas un
                    // probleme. On ne cree pas d'entree par mod demandeur,
                    // seulement une ligne dans la liste.
                    if (std::find(r.suggestions.begin(), r.suggestions.end(),
                                  d.id) == r.suggestions.end())
                        r.suggestions.push_back(d.id);
                }
                continue;
            }
            // Present : la version convient-elle ?
            if (d.range.empty() || d.range == "*") continue;
            bool ok = false;
            std::string found;
            for (const auto* dep : it->second) {
                if (dep->version.empty()) {
                    ok = true; // version inconnue : on n'accuse pas
                    break;
                }
                found = dep->version;
                if (modmeta::version_matches(dep->version, d.range)) {
                    ok = true;
                    break;
                }
            }
            if (!ok)
                add(r, Severity::Warning, m.file,
                    "Version de dépendance inattendue : " + d.id,
                    "Ce mod demande « " + d.id + " " + d.range +
                        " » ; la version installée est " + found + ".");
        }
    }

    // --- Doublons ------------------------------------------------------------
    // Deux jars qui declarent le meme modId : le chargeur en refuse un, ou
    // pire, en charge un au hasard. C'est le conflit le plus courant apres
    // une mise a jour manuelle ou l'import d'un modpack par-dessus l'autre.
    for (const auto& [id, list] : byId) {
        if (list.size() < 2) continue;
        std::string files;
        for (const auto* m : list) {
            if (!files.empty()) files += ", ";
            files += m->file;
            if (!m->version.empty()) files += " (" + m->version + ")";
        }
        add(r, Severity::Error, list[0]->file, "Mod présent en double : " + id,
            "Ces fichiers déclarent tous le même identifiant : " + files +
                ". Le chargeur refusera de démarrer. Gardez-en un seul.");
    }

    // Trie le plus grave en premier — ce qu'on veut lire d'abord.
    std::stable_sort(r.issues.begin(), r.issues.end(),
                     [](const Issue& a, const Issue& b) {
                         return static_cast<int>(a.sev) > static_cast<int>(b.sev);
                     });
    std::sort(r.suggestions.begin(), r.suggestions.end());
    return r;
}

Report scan(const std::filesystem::path& modsDir, const std::string& loader,
            const std::string& mcVersion) {
    return analyse(modmeta::read_dir(modsDir), loader, mcVersion);
}

} // namespace tl::modcheck

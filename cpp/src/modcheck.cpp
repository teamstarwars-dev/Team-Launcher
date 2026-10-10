#include "modcheck.hpp"

#include <algorithm>
#include <cctype>
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
    // Insensible a la casse : mcmod.info ecrit volontiers « Forge@[14.23,) ».
    // Compare tel quel, « Forge » n'etait pas reconnu comme le chargeur
    // lui-meme, et WorldEdit reclamait une « dependance manquante : Forge »
    // dans une instance… Forge.
    std::string low;
    for (char c : id)
        low.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return kPlatform.count(low) != 0;
}

// La version du jeu satisfait-elle la contrainte du mod ? Comme
// version_matches, a une nuance pres : dans mcmod.info, le champ
// « mcversion » est informel, et « 1.12 » y designe toute la branche 1.12
// — c'est ainsi que l'ecrivaient Chisels & Bits ou CraftStudioAPI, prevus
// pour 1.12.2. Fabric, lui, entend « 1.12 » au sens strict.
bool mc_matches(const modmeta::Mod& m, const std::string& version) {
    if (m.source == "mcmod.info") {
        const std::string& r = m.mcRange;
        bool bare = !r.empty() && r.size() <= 8;
        int dots = 0;
        for (char c : r) {
            if (c == '.') ++dots;
            else if (!std::isdigit(static_cast<unsigned char>(c))) bare = false;
        }
        if (bare && dots == 1)
            return version == r || version.rfind(r + ".", 0) == 0;
    }
    return modmeta::version_matches(version, m.mcRange);
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

    // --- Version de l'instance elle-meme ----------------------------------
    // Une version mal saisie (« 112.2 ») faisait signaler CHAQUE mod comme
    // incompatible : 39 lignes rouges pour une seule faute de frappe, et
    // aucune ne nommait la vraie cause. On la signale une fois, et l'on
    // saute la comparaison mod par mod, qui n'aurait aucun sens.
    //
    // « latest » n'est pas une erreur, mais on ne peut pas savoir hors
    // ligne a quelle version il correspond : comparer serait inventer.
    bool checkVersions = !mcVersion.empty() && mcVersion != "latest";
    if (checkVersions && !modmeta::is_mc_version(mcVersion)) {
        const std::string fix = modmeta::suggest_mc_version(mcVersion);
        add(r, Severity::Error, "", "Version de Minecraft invalide",
            "L'instance est réglée sur « " + mcVersion +
                " », qui n'est pas une version de Minecraft" +
                (fix.empty() ? std::string(".")
                             : " — vouliez-vous « " + fix + " » ?") +
                " Corrigez-la dans les paramètres de l'instance.");
        checkVersions = false;
    }

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
            // En Forge 1.12, un jar SANS manifeste est courant : coremods,
            // tweakers et bibliotheques (BetterFps, Essential,
            // MixinBootstrap…) sont decouverts par leur MANIFEST.MF ou leurs
            // annotations, pas par mcmod.info. Leur annoncer un
            // « telechargement incomplet » etait faux et inquietant. On le
            // signale a titre d'information seulement.
            //
            // Une archive reellement ILLISIBLE, elle, reste un avertissement
            // quelle que soit la version.
            const bool noManifest =
                m.readError.find("aucun manifeste") != std::string::npos;
            if (noManifest && legacy_forge(loader, mcVersion))
                add(r, Severity::Info, m.file, "Sans fiche de mod",
                    "Pas de mcmod.info dans ce jar : c'est courant en Forge "
                    "1.12 (coremod, tweaker, bibliothèque). Sa compatibilité "
                    "ne peut pas être vérifiée, rien d'anormal pour autant.");
            else
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
        if (checkVersions && !m.mcRange.empty() && !mc_matches(m, mcVersion)) {
            if (m.foreign)
                // Le manifeste lu n'est pas celui que le chargeur de
                // l'instance consulte : il ne prouve rien. Un mod 1.12.2
                // embarquant un mods.toml oublie fonctionne tres bien ; un
                // vrai mod 1.15 echouerait. On ne peut pas trancher, donc
                // on avertit sans bloquer.
                add(r, Severity::Warning, m.file,
                    "Version non vérifiable",
                    "Ce mod ne déclare sa version (« " + m.mcRange +
                        " ») que dans " + m.source +
                        ", un format que Forge " + mcVersion +
                        " ignore. Vérifiez qu'il est bien prévu pour " +
                        mcVersion + ".");
            else
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

// Forge 1.12 ou anterieur : ces versions ne lisent que mcmod.info.
bool legacy_forge(const std::string& loader, const std::string& mcVersion) {
    if (modmeta::loader_from_string(loader) != modmeta::Loader::Forge) return false;
    if (mcVersion.rfind("1.", 0) != 0) return false;
    std::size_t i = 2;
    int minor = 0;
    bool any = false;
    while (i < mcVersion.size() &&
           std::isdigit(static_cast<unsigned char>(mcVersion[i]))) {
        minor = minor * 10 + (mcVersion[i] - '0');
        any = true;
        if (minor > 99) return false;
        ++i;
    }
    return any && minor <= 12;
}

Report scan(const std::filesystem::path& modsDir, const std::string& loader,
            const std::string& mcVersion) {
    return analyse(modmeta::read_dir(modsDir, legacy_forge(loader, mcVersion)),
                   loader, mcVersion);
}

} // namespace tl::modcheck

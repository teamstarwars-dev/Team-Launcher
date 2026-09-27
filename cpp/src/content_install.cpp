#include "content_install.hpp"

#include "datastore.hpp"
#include "game_launcher.hpp" // latest_release
#include "modrinth.hpp"
#include "pack_import.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::content {

namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

bool iequals(const std::string& a, const std::string& b) {
    return a.size() == b.size() && lower(a) == lower(b);
}

} // namespace

const char* category_key(Category c) {
    switch (c) {
    case Category::Modpacks: return "modpack";
    case Category::Shaders: return "shader";
    default: return "mod";
    }
}

int curseforge_class(Category c) {
    switch (c) {
    case Category::Modpacks: return cf::kClassModpacks;
    case Category::Shaders: return cf::kClassShaders;
    default: return cf::kClassMods;
    }
}

const char* dest_subdir(Category c) {
    return c == Category::Shaders ? "shaderpacks" : "mods";
}

std::optional<std::string> resolve_loader(const json& inst) {
    const std::string l = lower(inst.value("Loader", std::string{}));
    if (l == "forge" || l == "fabric" || l == "neoforge") return l;
    // Quilt est compatible avec les mods Fabric : c'est ce que fait le C#.
    if (l == "quilt") return std::string("fabric");
    return std::nullopt; // Vanilla (ou inconnu) : pas de mods
}

std::string resolve_mc_version(const json& inst) {
    const std::string v = inst.value("McVersion", std::string{});
    if (!v.empty() && v != "latest" && v != "?") return v;
    return latest_release().value_or(std::string{});
}

const cf::File* pick_compatible(const std::vector<cf::File>& files,
                                const std::string& mcVersion,
                                const std::string& loader) {
    for (const auto& f : files) {
        const bool versionOk =
            std::any_of(f.gameVersions.begin(), f.gameVersions.end(),
                        [&](const std::string& g) { return iequals(g, mcVersion); });
        if (!versionOk) continue;
        // Un fichier sans loader declare est accepte (cf. C#) : beaucoup de
        // shaders et de mods universels n'en listent aucun.
        const bool loaderOk =
            loader.empty() || f.loaders.empty() ||
            std::any_of(f.loaders.begin(), f.loaders.end(),
                        [&](const std::string& l) { return iequals(l, loader); });
        if (loaderOk) return &f;
    }
    return nullptr;
}

namespace {

// Dossier temporaire pour les archives de modpack, nettoye apres import.
fs::path temp_pack_dir() {
    std::error_code ec;
    const fs::path d = fs::temp_directory_path(ec) / "teamlauncher-packs";
    fs::create_directories(d, ec);
    return d;
}

Outcome import_downloaded_pack(const fs::path& archive, std::atomic<bool>& cancel) {
    Outcome o;
    auto r = packs::import_any(archive, nullptr, cancel);
    std::error_code ec;
    fs::remove(archive, ec);
    if (r.cancelled) {
        o.error = "Installation annulée.";
        return o;
    }
    if (!r.error.empty()) {
        o.error = r.error;
        return o;
    }
    o.ok = true;
    o.instance = r.instance;
    return o;
}

} // namespace

Outcome install(bool fromCurseForge, const std::string& slugOrId, const json& inst,
                Category cat, std::atomic<bool>& cancel) {
    Outcome o;
    try {
        // --- Modpack : on telecharge l'archive puis on l'importe en instance ---
        if (cat == Category::Modpacks) {
            if (!fromCurseForge) {
                const std::string file = mr::download_project_file(
                    slugOrId, temp_pack_dir(), {}, {}, &cancel);
                return import_downloaded_pack(file, cancel);
            }
            const auto files = cf::get_files(std::atoi(slugOrId.c_str()), &cancel);
            if (files.empty()) {
                o.error = "Aucun fichier trouvé sur CurseForge pour ce projet.";
                return o;
            }
            // Le plus recent (get_files trie par date decroissante).
            const std::string file =
                cf::download_file(files.front(), temp_pack_dir(), &cancel);
            if (file.empty()) {
                o.error = "Téléchargement du modpack CurseForge impossible.";
                return o;
            }
            return import_downloaded_pack(file, cancel);
        }

        // --- Mod / shader : il faut une instance cible ---
        if (!inst.is_object() || inst.value("Id", std::string{}).empty()) {
            o.error = "Choisis d'abord une instance de destination.";
            return o;
        }
        const std::string mcVersion = resolve_mc_version(inst);
        if (mcVersion.empty()) {
            o.error = "Impossible de déterminer la version de Minecraft de "
                      "l'instance (manifeste Mojang injoignable).";
            return o;
        }
        // Le loader n'est exige que pour les mods : un shader s'installe sur
        // n'importe quelle instance.
        std::string loader;
        if (cat == Category::Mods) {
            auto l = resolve_loader(inst);
            if (!l) {
                o.error = "Cette instance est en Vanilla : définis d'abord un "
                          "chargeur de mods (Forge, Fabric ou NeoForge) dans "
                          "ses réglages.";
                return o;
            }
            loader = *l;
        }

        const fs::path destDir = DataStore::instancesRoot() /
                                 inst.value("Id", std::string{}) / dest_subdir(cat);

        if (!fromCurseForge) {
            o.installed =
                mr::download_project_file(slugOrId, destDir, loader, mcVersion, &cancel);
            o.ok = true;
            return o;
        }

        const auto files = cf::get_files(std::atoi(slugOrId.c_str()), &cancel);
        if (files.empty()) {
            o.error = "Aucun fichier trouvé sur CurseForge pour ce projet.";
            return o;
        }
        const cf::File* best = pick_compatible(files, mcVersion, loader);
        if (!best) {
            o.error = "Aucun fichier CurseForge compatible " +
                      inst.value("Loader", std::string{"?"}) + " " + mcVersion +
                      ".\nVérifie la version de l'instance ou choisis un autre "
                      "contenu.";
            return o;
        }
        o.installed = cf::download_file(*best, destDir, &cancel);
        if (o.installed.empty()) {
            o.error = "Téléchargement CurseForge impossible.";
            return o;
        }
        o.ok = true;
        return o;
    } catch (const std::exception& ex) {
        o.error = ex.what();
        return o;
    }
}

} // namespace tl::content

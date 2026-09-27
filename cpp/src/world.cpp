#include "world.hpp"

#include "nbt.hpp"
#include "region.hpp"

#include <algorithm>

namespace fs = std::filesystem;

namespace tl::world {

namespace {

// Quelques reperes DataVersion -> version publique. Liste volontairement
// courte : elle sert a afficher un repere, pas a decider d'un comportement
// (le code qui depend de la version teste DataVersion directement).
struct VerPoint {
    std::int64_t dv;
    const char* name;
};
constexpr VerPoint kVersions[] = {
    {4325, "1.21.4"}, {3953, "1.21"},   {3837, "1.20.6"}, {3463, "1.20.1"},
    {3337, "1.19.4"}, {3105, "1.19"},   {2975, "1.18.2"}, {2860, "1.18"},
    {2730, "1.17.1"}, {2724, "1.17"},   {2586, "1.16.5"}, {2566, "1.16.4"},
    {2230, "1.15.2"}, {1976, "1.14.4"}, {1631, "1.13.2"}, {1343, "1.12.2"},
    {1139, "1.12"},   {922,  "1.11.2"}, {819,  "1.11"},   {510,  "1.10.2"},
    {184,  "1.9.4"},
};

} // namespace

std::string version_name(std::int64_t dataVersion) {
    if (dataVersion <= 0) return {};
    for (const auto& v : kVersions) {
        if (dataVersion == v.dv) return v.name;
        // Entre deux reperes : on annonce le plus proche en dessous, prefixe.
        if (dataVersion > v.dv) return std::string("~") + v.name;
    }
    return "<= 1.9";
}

std::optional<Info> read_level(const fs::path& worldDir) {
    std::error_code ec;
    const fs::path lvl = worldDir / "level.dat";
    if (!fs::is_regular_file(lvl, ec)) return std::nullopt;

    auto root = nbt::read_file(lvl.string());
    if (!root) return std::nullopt;
    // level.dat : racine (souvent nommee "") contenant un compound « Data ».
    const nbt::Compound* data = nbt::get_compound(*root, "Data");
    if (!data) return std::nullopt;

    Info w;
    w.path = worldDir;
    w.folder = worldDir.filename().string();
    w.name = nbt::get_string(*data, "LevelName", w.folder);
    w.lastPlayed = nbt::get_num(*data, "LastPlayed", 0);
    w.dataVersion = nbt::get_num(*data, "DataVersion", 0);
    // 1.16+ : la graine a migre dans WorldGenSettings.
    w.seed = nbt::get_num(*data, "RandomSeed", 0);
    if (w.seed == 0)
        if (const nbt::Compound* wgs = nbt::get_compound(*data, "WorldGenSettings"))
            w.seed = nbt::get_num(*wgs, "seed", 0);
    w.gameType = static_cast<int>(nbt::get_num(*data, "GameType", -1));
    w.hardcore = nbt::get_num(*data, "hardcore", 0) != 0;
    w.sizeBytes = dir_size(worldDir);
    w.regionCount = static_cast<int>(region::list_regions(worldDir).size());
    return w;
}

std::vector<Info> list_worlds(const fs::path& instanceDir) {
    std::vector<Info> out;
    std::error_code ec;
    const fs::path saves = instanceDir / "saves";
    if (!fs::is_directory(saves, ec)) return out;
    for (const auto& e : fs::directory_iterator(saves, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
        if (auto w = read_level(e.path())) out.push_back(std::move(*w));
    }
    // Plus recemment joue d'abord ; les mondes sans date passent a la fin.
    std::sort(out.begin(), out.end(), [](const Info& a, const Info& b) {
        if (a.lastPlayed != b.lastPlayed) return a.lastPlayed > b.lastPlayed;
        return a.folder < b.folder;
    });
    return out;
}

int count_empty_regions(const fs::path& worldDir) {
    int n = 0;
    std::error_code ec;
    const fs::path dir = worldDir / "region";
    if (!fs::is_directory(dir, ec)) return 0;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".mca") continue;
        // <= 8192 : l'en-tete seul, aucun chunk stocke.
        if (fs::file_size(e.path(), ec) <= 8192 && !ec) ++n;
    }
    return n;
}

int delete_empty_regions(const fs::path& worldDir) {
    int n = 0;
    std::error_code ec;
    const fs::path dir = worldDir / "region";
    if (!fs::is_directory(dir, ec)) return 0;
    std::vector<fs::path> doomed;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (e.path().extension() != ".mca") continue;
        if (fs::file_size(e.path(), ec) <= 8192 && !ec) doomed.push_back(e.path());
    }
    // Suppression apres le parcours : modifier un dossier pendant qu'on
    // l'itere est un comportement indefini.
    for (const auto& p : doomed)
        if (fs::remove(p, ec) && !ec) ++n;
    return n;
}

std::int64_t dir_size(const fs::path& p) {
    std::int64_t total = 0;
    std::error_code ec;
    if (!fs::is_directory(p, ec)) return 0;
    for (fs::recursive_directory_iterator it(p, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        std::error_code e2;
        if (it->is_regular_file(e2))
            total += static_cast<std::int64_t>(fs::file_size(it->path(), e2));
    }
    return total;
}

} // namespace tl::world

#include "presets.hpp"

#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::presets {

namespace {

constexpr const char* kSuffix = ".disabled";

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool is_disabled(const std::string& fileName) {
    const std::string l = lower(fileName);
    return l.size() > 9 && l.compare(l.size() - 9, 9, kSuffix) == 0;
}

// « sodium.jar.disabled » -> « sodium.jar »
std::string base_name(const std::string& fileName) {
    return is_disabled(fileName)
               ? fileName.substr(0, fileName.size() - 9)
               : fileName;
}

// Un fichier de mod : .jar ou .jar.disabled. On ignore le reste (README,
// dossiers de configuration deposes dans mods/...).
bool is_mod(const std::string& fileName) {
    const std::string b = lower(base_name(fileName));
    return b.size() > 4 && b.compare(b.size() - 4, 4, ".jar") == 0;
}

} // namespace

std::vector<Preset> load(const json& inst) {
    std::vector<Preset> out;
    const auto it = inst.find("Presets");
    if (it == inst.end() || !it->is_array()) return out;
    for (const auto& j : *it) {
        if (!j.is_object()) continue;
        Preset p;
        p.name = j.value("Name", "");
        if (p.name.empty()) continue;
        p.ramGb = j.value("RamGb", 0);
        p.jvmArgs = j.value("JvmArgs", "");
        p.hasMods = j.value("HasMods", false);
        if (const auto d = j.find("Disabled"); d != j.end() && d->is_array())
            for (const auto& x : *d)
                if (x.is_string()) p.disabled.push_back(x.get<std::string>());
        out.push_back(std::move(p));
    }
    return out;
}

void store(json& inst, const std::vector<Preset>& v) {
    json arr = json::array();
    for (const auto& p : v) {
        json j = {{"Name", p.name},
                  {"RamGb", p.ramGb},
                  {"JvmArgs", p.jvmArgs},
                  {"HasMods", p.hasMods}};
        if (p.hasMods) j["Disabled"] = p.disabled;
        arr.push_back(std::move(j));
    }
    if (arr.empty())
        inst.erase("Presets"); // pas de tableau vide qui traine
    else
        inst["Presets"] = arr;
}

std::string active(const json& inst) { return inst.value("ActivePreset", ""); }

void set_active(json& inst, const std::string& name) {
    if (name.empty())
        inst.erase("ActivePreset");
    else
        inst["ActivePreset"] = name;
}

std::vector<std::string> all_mods(const fs::path& modsDir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return out;
    for (fs::directory_iterator it(modsDir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string fn = it->path().filename().string();
        if (!is_mod(fn)) continue;
        out.push_back(base_name(fn));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> current_disabled(const fs::path& modsDir) {
    std::vector<std::string> out;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return out;
    for (fs::directory_iterator it(modsDir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const std::string fn = it->path().filename().string();
        if (!is_mod(fn) || !is_disabled(fn)) continue;
        out.push_back(base_name(fn));
    }
    std::sort(out.begin(), out.end());
    return out;
}

ApplyResult apply_mods(const fs::path& modsDir,
                       const std::vector<std::string>& disabled,
                       const std::vector<std::string>& known) {
    ApplyResult r;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return r;

    auto contains = [](const std::vector<std::string>& v,
                       const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };

    // Instantane d'abord : on ne parcourt pas un dossier qu'on renomme.
    std::vector<fs::path> files;
    for (fs::directory_iterator it(modsDir, ec), end; it != end;
         it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        if (!is_mod(it->path().filename().string())) continue;
        files.push_back(it->path());
    }

    for (const auto& p : files) {
        const std::string fn = p.filename().string();
        const std::string base = base_name(fn);
        const bool nowOff = is_disabled(fn);

        // Mod absent du profil : il a ete installe APRES la capture. On ne
        // le desactive pas d'office — sinon un mod tout juste ajoute
        // disparaitrait en silence a la premiere application d'un profil.
        if (!contains(known, base)) {
            ++r.untouched;
            continue;
        }

        const bool wantOff = contains(disabled, base);
        if (wantOff == nowOff) continue;

        std::error_code re;
        if (wantOff)
            fs::rename(p, fs::path(p.string() + kSuffix), re);
        else
            fs::rename(p, p.parent_path() / base, re);
        if (re)
            ++r.failed;
        else if (wantOff)
            ++r.disabled;
        else
            ++r.enabled;
    }
    return r;
}

} // namespace tl::presets

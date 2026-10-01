#include "modmeta.hpp"

#include "util_zip.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <system_error>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::modmeta {

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

std::string unquote(const std::string& s) {
    const std::string t = trim(s);
    if (t.size() >= 2 && ((t.front() == '"' && t.back() == '"') ||
                          (t.front() == '\'' && t.back() == '\'')))
        return t.substr(1, t.size() - 2);
    return t;
}

// Texte d'un champ JSON qui peut etre une chaine ou autre chose : on ne
// veut jamais lancer, un manifeste mal forme est frequent.
std::string str_of(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return std::to_string(it->get<long long>());
    return {};
}

// Une contrainte Fabric peut etre une chaine ou un tableau de chaines
// (alternatives). On les recolle avec « || », que version_matches sait lire.
std::string range_of(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_array()) {
        std::string out;
        for (const auto& e : v) {
            if (!e.is_string()) continue;
            if (!out.empty()) out += " || ";
            out += e.get<std::string>();
        }
        return out;
    }
    return {};
}

} // namespace

const char* loader_name(Loader l) {
    switch (l) {
    case Loader::Fabric: return "Fabric";
    case Loader::Quilt: return "Quilt";
    case Loader::Forge: return "Forge";
    case Loader::NeoForge: return "NeoForge";
    default: return "Inconnu";
    }
}

Loader loader_from_string(const std::string& s) {
    std::string t;
    for (char c : s) t.push_back(static_cast<char>(std::tolower(
                                    static_cast<unsigned char>(c))));
    if (t.find("neoforge") != std::string::npos) return Loader::NeoForge;
    if (t.find("quilt") != std::string::npos) return Loader::Quilt;
    if (t.find("fabric") != std::string::npos) return Loader::Fabric;
    if (t.find("forge") != std::string::npos) return Loader::Forge;
    return Loader::Unknown;
}

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

namespace {

// Decoupe « 1.20.1-rc2 » en ({1,20,1}, "rc2").
void split_version(const std::string& v, std::vector<long long>& nums,
                   std::string& suffix) {
    nums.clear();
    suffix.clear();
    size_t i = 0;
    while (i < v.size()) {
        if (std::isdigit(static_cast<unsigned char>(v[i]))) {
            long long n = 0;
            // Garde-fou : une suite de chiffres absurde (date, empreinte)
            // ne doit pas deborder.
            int digits = 0;
            while (i < v.size() &&
                   std::isdigit(static_cast<unsigned char>(v[i]))) {
                if (digits < 15) n = n * 10 + (v[i] - '0');
                ++digits;
                ++i;
            }
            nums.push_back(n);
            if (i < v.size() && (v[i] == '.' || v[i] == '_')) {
                ++i;
                continue;
            }
            // Fin des composantes numeriques : le reste est un suffixe.
            if (i < v.size()) {
                suffix = v.substr(i);
                if (!suffix.empty() && (suffix[0] == '-' || suffix[0] == '+'))
                    suffix.erase(0, 1);
            }
            return;
        }
        // Pas de chiffre en tete : tout est suffixe.
        suffix = v.substr(i);
        return;
    }
}

} // namespace

int compare_versions(const std::string& a, const std::string& b) {
    std::vector<long long> na, nb;
    std::string sa, sb;
    split_version(trim(a), na, sa);
    split_version(trim(b), nb, sb);
    const size_t n = std::max(na.size(), nb.size());
    for (size_t i = 0; i < n; ++i) {
        const long long x = i < na.size() ? na[i] : 0;
        const long long y = i < nb.size() ? nb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    // Semver : « 1.0 » est PLUS RECENT que « 1.0-beta1 ». Un suffixe
    // marque une preversion, pas un incrément.
    if (sa.empty() != sb.empty()) return sa.empty() ? 1 : -1;
    if (sa == sb) return 0;
    return sa < sb ? -1 : 1;
}

namespace {

// Une contrainte elementaire : « >=1.20 », « 1.20.1 », « [1.20,1.21) »...
bool single_match(const std::string& version, const std::string& c) {
    const std::string t = trim(c);
    if (t.empty() || t == "*" || t == "any") return true;

    // Intervalle Maven : [a,b] [a,b) (a,b] (a,b) [a] et bornes vides.
    if ((t.front() == '[' || t.front() == '(') &&
        (t.back() == ']' || t.back() == ')')) {
        const bool loIncl = t.front() == '[';
        const bool hiIncl = t.back() == ']';
        const std::string inner = t.substr(1, t.size() - 2);
        const size_t comma = inner.find(',');
        if (comma == std::string::npos) {
            // [1.20] : egalite stricte. (1.20) n'a pas de sens, on l'accepte
            // comme egalite plutot que de refuser.
            return compare_versions(version, trim(inner)) == 0;
        }
        const std::string lo = trim(inner.substr(0, comma));
        const std::string hi = trim(inner.substr(comma + 1));
        if (!lo.empty()) {
            const int r = compare_versions(version, lo);
            if (r < 0 || (r == 0 && !loIncl)) return false;
        }
        if (!hi.empty()) {
            const int r = compare_versions(version, hi);
            if (r > 0 || (r == 0 && !hiIncl)) return false;
        }
        return true;
    }

    // Operateurs.
    auto starts = [&](const char* p) { return t.rfind(p, 0) == 0; };
    if (starts(">=")) return compare_versions(version, trim(t.substr(2))) >= 0;
    if (starts("<=")) return compare_versions(version, trim(t.substr(2))) <= 0;
    if (starts("==")) return compare_versions(version, trim(t.substr(2))) == 0;
    if (starts(">")) return compare_versions(version, trim(t.substr(1))) > 0;
    if (starts("<")) return compare_versions(version, trim(t.substr(1))) < 0;
    if (starts("=")) return compare_versions(version, trim(t.substr(1))) == 0;

    // ~1.20.1 : meme majeure et meme mineure, correctif superieur ou egal.
    // ^1.20.1 : meme majeure.
    if (starts("~") || starts("^")) {
        const bool tilde = t[0] == '~';
        const std::string base = trim(t.substr(1));
        if (compare_versions(version, base) < 0) return false;
        std::vector<long long> nv, nb;
        std::string sv, sb;
        split_version(version, nv, sv);
        split_version(base, nb, sb);
        if (nb.empty() || nv.empty()) return true;
        if (nv[0] != nb[0]) return false;
        if (tilde) {
            const long long mv = nv.size() > 1 ? nv[1] : 0;
            const long long mb = nb.size() > 1 ? nb[1] : 0;
            if (mv != mb) return false;
        }
        return true;
    }

    // Aucun chiffre : ce n'est pas une version, donc pas une contrainte
    // qu'on sache evaluer (« latest », « any_version », une syntaxe d'un
    // chargeur qu'on ne connait pas). On accepte : bloquer un lancement
    // sur une chaine qu'on n'a pas comprise serait pire que se taire.
    if (t.find_first_of("0123456789") == std::string::npos) return true;

    // Version nue : Fabric l'entend comme une egalite. Mais un joker de
    // fin (« 1.20.x », « 1.20.* ») veut dire « toute la branche ».
    if (t.size() >= 2) {
        const std::string tail = t.substr(t.size() - 2);
        if (tail == ".x" || tail == ".*") {
            const std::string prefix = t.substr(0, t.size() - 1); // garde le point
            return version.rfind(prefix, 0) == 0 || version + "." == prefix;
        }
    }
    return compare_versions(version, t) == 0;
}

} // namespace

bool version_matches(const std::string& version, const std::string& range) {
    const std::string r = trim(range);
    if (r.empty() || r == "*" || r == "any") return true;
    if (trim(version).empty()) return true; // rien a comparer : on n'accuse pas

    // Alternatives : « a || b ». Une seule suffit.
    size_t bar = r.find("||");
    if (bar != std::string::npos) {
        size_t start = 0;
        while (true) {
            const std::string part =
                r.substr(start, bar == std::string::npos ? std::string::npos
                                                         : bar - start);
            if (version_matches(version, part)) return true;
            if (bar == std::string::npos) break;
            start = bar + 2;
            bar = r.find("||", start);
        }
        return false;
    }

    // Conjonction : contraintes separees par des virgules ou des espaces.
    // Attention, une virgule DANS un intervalle Maven n'en est pas un
    // separateur : on ne coupe qu'en dehors des crochets.
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    for (char c : r) {
        if (c == '[' || c == '(') ++depth;
        if (c == ']' || c == ')') --depth;
        if (depth <= 0 && (c == ',' || c == ' ' || c == '\t')) {
            // Un operateur isole (« >= 1.20 ») ne doit pas etre coupe de son
            // operande : on ne ferme la partie que si elle a une valeur.
            const std::string t = trim(cur);
            if (!t.empty() && t != ">=" && t != "<=" && t != ">" && t != "<" &&
                t != "=" && t != "==") {
                parts.push_back(t);
                cur.clear();
                continue;
            }
            if (t.empty()) {
                cur.clear();
                continue;
            }
            cur = t; // operateur en attente de son operande
            continue;
        }
        cur.push_back(c);
    }
    if (!trim(cur).empty()) parts.push_back(trim(cur));
    if (parts.empty()) return true;
    for (const auto& p : parts)
        if (!single_match(version, p)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// Analyseurs
// ---------------------------------------------------------------------------

Mod parse_fabric(const std::string& text) {
    Mod m;
    m.loader = Loader::Fabric;
    json j;
    try {
        j = json::parse(text, nullptr, true, /*ignore_comments=*/true);
    } catch (const std::exception& ex) {
        m.readError = std::string("fabric.mod.json illisible : ") + ex.what();
        return m;
    }
    if (!j.is_object()) {
        m.readError = "fabric.mod.json : objet attendu";
        return m;
    }
    m.id = str_of(j, "id");
    m.name = str_of(j, "name");
    m.version = str_of(j, "version");

    auto collect = [&](const char* key, bool mandatory) {
        auto it = j.find(key);
        if (it == j.end() || !it->is_object()) return;
        for (auto d = it->begin(); d != it->end(); ++d) {
            const std::string id = d.key();
            const std::string range = range_of(d.value());
            if (id == "minecraft") {
                if (mandatory) m.mcRange = range;
                continue;
            }
            // `java` est fourni par le JRE, pas par un mod : l'exiger dans
            // la liste des mods installes produirait un faux manquant.
            if (id == "java") continue;
            m.deps.push_back(Dep{id, mandatory, range});
        }
    };
    collect("depends", true);
    collect("recommends", false);
    collect("suggests", false);
    return m;
}

Mod parse_quilt(const std::string& text) {
    Mod m;
    m.loader = Loader::Quilt;
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& ex) {
        m.readError = std::string("quilt.mod.json illisible : ") + ex.what();
        return m;
    }
    // Tout vit sous « quilt_loader ».
    const json ql = j.is_object() && j.contains("quilt_loader")
                        ? j["quilt_loader"]
                        : json::object();
    if (!ql.is_object()) {
        m.readError = "quilt.mod.json : bloc quilt_loader attendu";
        return m;
    }
    m.id = str_of(ql, "id");
    m.version = str_of(ql, "version");
    if (ql.contains("metadata") && ql["metadata"].is_object())
        m.name = str_of(ql["metadata"], "name");

    // « depends » est ici un TABLEAU d'objets {id, versions, optional}.
    auto it = ql.find("depends");
    if (it != ql.end() && it->is_array()) {
        for (const auto& d : *it) {
            std::string id, range;
            bool optional = false;
            if (d.is_string()) {
                id = d.get<std::string>();
            } else if (d.is_object()) {
                id = str_of(d, "id");
                if (d.contains("versions")) range = range_of(d["versions"]);
                if (d.contains("optional") && d["optional"].is_boolean())
                    optional = d["optional"].get<bool>();
            }
            if (id.empty()) continue;
            if (id == "minecraft") {
                m.mcRange = range;
                continue;
            }
            if (id == "java" || id == "quilt_loader") continue;
            m.deps.push_back(Dep{id, !optional, range});
        }
    }
    return m;
}

Mod parse_mcmod_info(const std::string& text) {
    Mod m;
    m.loader = Loader::Forge;
    json j;
    try {
        j = json::parse(text, nullptr, true, true);
    } catch (const std::exception& ex) {
        m.readError = std::string("mcmod.info illisible : ") + ex.what();
        return m;
    }
    // Deux formes coexistent : un tableau, ou {modListVersion, modList:[]}.
    const json* arr = nullptr;
    if (j.is_array())
        arr = &j;
    else if (j.is_object() && j.contains("modList") && j["modList"].is_array())
        arr = &j["modList"];
    if (!arr || arr->empty()) {
        m.readError = "mcmod.info : aucune entree";
        return m;
    }
    const json& e = (*arr)[0];
    if (!e.is_object()) {
        m.readError = "mcmod.info : objet attendu";
        return m;
    }
    m.id = str_of(e, "modid");
    m.name = str_of(e, "name");
    m.version = str_of(e, "version");
    m.mcRange = str_of(e, "mcversion");
    auto deps = e.find("requiredMods");
    if (deps != e.end() && deps->is_array())
        for (const auto& d : *deps) {
            if (!d.is_string()) continue;
            // Forme « modid@[1.0,) ».
            std::string s = d.get<std::string>();
            const size_t at = s.find('@');
            Dep dep;
            dep.id = at == std::string::npos ? s : s.substr(0, at);
            if (at != std::string::npos) dep.range = s.substr(at + 1);
            if (dep.id.empty() || dep.id == "minecraft" || dep.id == "forge")
                continue;
            m.deps.push_back(dep);
        }
    return m;
}

Mod parse_mods_toml(const std::string& text, bool neoforge) {
    Mod m;
    m.loader = neoforge ? Loader::NeoForge : Loader::Forge;

    // Analyseur TOML volontairement minimal : ces fichiers sont ecrits a la
    // main par les moddeurs et n'utilisent qu'une poignee de constructions
    // (tables, tableaux de tables, chaines, booleens). Embarquer une
    // bibliotheque TOML complete pour cela couterait plus que ce qu'elle
    // rapporterait. Ce qu'on ne comprend pas est ignore, jamais devine.
    std::string table;            // table courante
    int modsSeen = 0;             // on ne retient que le premier [[mods]]
    std::string depOwner;         // <x> dans [[dependencies.<x>]]
    Dep curDep;
    bool inDep = false;

    auto flush_dep = [&] {
        if (!inDep) return;
        inDep = false;
        if (curDep.id.empty()) return;
        if (curDep.id == "minecraft") {
            if (m.mcRange.empty()) m.mcRange = curDep.range;
            return;
        }
        if (curDep.id == "forge" || curDep.id == "neoforge" ||
            curDep.id == "java")
            return;
        m.deps.push_back(curDep);
    };

    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line =
            text.substr(pos, nl == std::string::npos ? std::string::npos
                                                     : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();

        // Retire le commentaire, en respectant les guillemets.
        bool inStr = false;
        char quote = 0;
        for (size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (inStr) {
                if (c == '\\') { ++i; continue; }
                if (c == quote) inStr = false;
            } else if (c == '"' || c == '\'') {
                inStr = true;
                quote = c;
            } else if (c == '#') {
                line = line.substr(0, i);
                break;
            }
        }
        line = trim(line);
        if (line.empty()) continue;

        if (line.front() == '[') {
            flush_dep();
            const bool arrayTable = line.rfind("[[", 0) == 0;
            size_t end = line.find(arrayTable ? "]]" : "]");
            if (end == std::string::npos) continue;
            table = trim(line.substr(arrayTable ? 2 : 1,
                                     end - (arrayTable ? 2 : 1)));
            if (arrayTable && table == "mods") {
                ++modsSeen;
            } else if (arrayTable && table.rfind("dependencies.", 0) == 0) {
                depOwner = table.substr(13);
                curDep = Dep{};
                inDep = true;
            }
            continue;
        }

        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string raw = trim(line.substr(eq + 1));
        const std::string val = unquote(raw);

        if (table.empty()) {
            // Racine : modLoader="javafml" / "lowcodefml", pas exploitable
            // pour distinguer Forge de NeoForge — c'est le NOM du fichier
            // qui tranche, d'ou le parametre.
            continue;
        }
        if (table == "mods" && modsSeen == 1) {
            if (key == "modId") m.id = val;
            else if (key == "version") m.version = val;
            else if (key == "displayName") m.name = val;
            continue;
        }
        if (inDep) {
            if (key == "modId") curDep.id = val;
            else if (key == "versionRange") curDep.range = val;
            else if (key == "mandatory" || key == "required")
                curDep.mandatory = raw.rfind("true", 0) == 0;
            else if (key == "type")
                // NeoForge 1.21 : type = "required" | "optional" |
                // "incompatible" | "discouraged". Remplace `mandatory`.
                curDep.mandatory = val == "required";
            continue;
        }
        (void)depOwner;
    }
    flush_dep();

    if (m.id.empty() && m.readError.empty())
        m.readError = "mods.toml : aucun modId";
    return m;
}

// ---------------------------------------------------------------------------
// Lecture d'archive
// ---------------------------------------------------------------------------

Mod read_jar(const fs::path& jar) {
    Mod m;
    m.file = jar.filename().string();
    m.disabled = m.file.size() >= 9 &&
                 m.file.compare(m.file.size() - 9, 9, ".disabled") == 0;

    std::error_code ec;
    if (!fs::is_regular_file(jar, ec)) {
        m.readError = "fichier introuvable";
        return m;
    }

    // Ordre d'essai : le plus specifique d'abord. Beaucoup de mods
    // multi-chargeurs embarquent PLUSIEURS manifestes dans le meme jar ;
    // celui de NeoForge prime sur celui de Forge, et Quilt sur Fabric,
    // parce que le plus recent decrit mieux ce que le jar sait faire.
    struct Attempt {
        const char* entry;
        Mod (*parse)(const std::string&);
    };
    if (auto s = zip_read_entry(jar, "META-INF/neoforge.mods.toml")) {
        m = parse_mods_toml(*s, /*neoforge=*/true);
    } else if (auto q = zip_read_entry(jar, "quilt.mod.json")) {
        m = parse_quilt(*q);
    } else if (auto f = zip_read_entry(jar, "fabric.mod.json")) {
        m = parse_fabric(*f);
    } else if (auto t = zip_read_entry(jar, "META-INF/mods.toml")) {
        m = parse_mods_toml(*t, /*neoforge=*/false);
    } else if (auto o = zip_read_entry(jar, "mcmod.info")) {
        m = parse_mcmod_info(*o);
    } else {
        m.readError = "aucun manifeste de mod dans l'archive";
    }

    m.file = jar.filename().string();
    m.disabled = m.file.size() >= 9 &&
                 m.file.compare(m.file.size() - 9, 9, ".disabled") == 0;
    if (m.name.empty()) m.name = m.id.empty() ? m.file : m.id;
    return m;
}

std::vector<Mod> read_dir(const fs::path& modsDir) {
    std::vector<Mod> out;
    std::error_code ec;
    if (!fs::is_directory(modsDir, ec)) return out;
    for (const auto& e : fs::directory_iterator(modsDir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        const std::string n = e.path().filename().string();
        const bool jar = n.size() > 4 && n.compare(n.size() - 4, 4, ".jar") == 0;
        const bool off = n.size() > 13 &&
                         n.compare(n.size() - 13, 13, ".jar.disabled") == 0;
        if (!jar && !off) continue;
        out.push_back(read_jar(e.path()));
    }
    std::sort(out.begin(), out.end(),
              [](const Mod& a, const Mod& b) { return a.file < b.file; });
    return out;
}

} // namespace tl::modmeta

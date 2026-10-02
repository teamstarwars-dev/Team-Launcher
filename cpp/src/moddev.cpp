#include "moddev.hpp"

#include "game_installer.hpp" // runtime_root
#include "game_launcher.hpp" // download_java (JDK)
#include "http_win.hpp"
#include "util_zip.hpp"      // zip_extract_all
#include "proc.hpp" // posix_spawn (POSIX) ; vide sous Windows

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
// Etape 5 (Linux) : sh -c + strcasecmp/read.
#include <strings.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::moddev {

namespace {

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
}
#endif

// Comparaison insensible a la casse : _stricmp (Win32) / strcasecmp (POSIX).
int strCaseCmp(const char* a, const char* b) {
#ifdef _WIN32
    return ::_stricmp(a, b);
#else
    return ::strcasecmp(a, b);
#endif
}

// Retire les diacritiques des lettres latines courantes : un nom de mod
// francais doit donner une classe Java valide (le C# ne le faisait pas).
std::string deaccent(const std::string& in) {
    static const std::map<std::string, char> kMap = {
        {"à", 'a'}, {"â", 'a'}, {"ä", 'a'}, {"á", 'a'}, {"ã", 'a'},
        {"ç", 'c'}, {"é", 'e'}, {"è", 'e'}, {"ê", 'e'}, {"ë", 'e'},
        {"î", 'i'}, {"ï", 'i'}, {"í", 'i'}, {"ô", 'o'}, {"ö", 'o'},
        {"ó", 'o'}, {"õ", 'o'}, {"ù", 'u'}, {"û", 'u'}, {"ü", 'u'},
        {"ú", 'u'}, {"ÿ", 'y'}, {"ñ", 'n'},
    };
    std::string out;
    for (std::size_t i = 0; i < in.size();) {
        // Les lettres accentuees latines tiennent sur 2 octets en UTF-8.
        if ((static_cast<unsigned char>(in[i]) & 0xE0) == 0xC0 &&
            i + 1 < in.size()) {
            const std::string two = in.substr(i, 2);
            std::string lower = two;
            const auto it = kMap.find(lower);
            if (it != kMap.end()) {
                out.push_back(it->second);
                i += 2;
                continue;
            }
            // Majuscule accentuee : le second octet est decale de 0x20.
            std::string asLower = two;
            asLower[1] = static_cast<char>(
                static_cast<unsigned char>(asLower[1]) + 0x20);
            const auto it2 = kMap.find(asLower);
            if (it2 != kMap.end()) {
                out.push_back(static_cast<char>(std::toupper(it2->second)));
                i += 2;
                continue;
            }
            i += 2;
            continue;
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

bool file_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

// Cherche un executable dans le PATH.
#ifdef _WIN32
fs::path which(const std::wstring& exe) {
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, buf, nullptr) > 0)
        return fs::path(buf);
    return {};
}
#else
fs::path which(const std::string& exe) {
    if (const char* p = std::getenv("PATH"); p && *p) {
        std::istringstream ps(p);
        std::string d;
        while (std::getline(ps, d, ':')) {
            if (d.empty()) continue;
            const fs::path cand = fs::path(d) / exe;
            std::error_code ec;
            if (fs::is_regular_file(cand, ec) &&
                ::access(cand.string().c_str(), X_OK) == 0)
                return cand;
        }
    }
    return {};
}
#endif

// Versions de repli : ce que le C# codait en dur, mais sous une forme qui
// existe reellement. Elles vieillissent — d'ou la resolution en ligne.
constexpr const char* kFallbackYarnSuffix = "+build.1";
constexpr const char* kFallbackLoader = "0.16.9";

} // namespace

const char* loader_name(Loader l) {
    switch (l) {
        case Loader::Fabric: return "Fabric";
        case Loader::Forge: return "Forge";
        case Loader::NeoForge: return "NeoForge";
        case Loader::Bedrock: return "Bedrock";
    }
    return "Fabric";
}

Loader loader_from(const std::string& s) {
    if (strCaseCmp(s.c_str(), "forge") == 0) return Loader::Forge;
    if (strCaseCmp(s.c_str(), "neoforge") == 0) return Loader::NeoForge;
    if (strCaseCmp(s.c_str(), "bedrock") == 0) return Loader::Bedrock;
    return Loader::Fabric;
}

const std::vector<std::string>& versions_for(Loader l) {
    static const std::vector<std::string> kFabric = {
        "1.21.5", "1.21.4", "1.21.3", "1.21.2", "1.21.1", "1.21",
        "1.20.6", "1.20.4", "1.20.2", "1.20.1", "1.20",
        "1.19.4", "1.19.3", "1.19.2", "1.18.2"};
    static const std::vector<std::string> kForge = {
        "1.21.5", "1.21.4", "1.21.3", "1.21.2", "1.21.1",
        "1.20.6", "1.20.4", "1.20.2", "1.20.1",
        "1.19.4", "1.19.3", "1.19.2", "1.18.2", "1.16.5"};
    static const std::vector<std::string> kNeo = {
        "1.21.5", "1.21.4", "1.21.3", "1.21.2", "1.21.1",
        "1.20.6", "1.20.4", "1.20.2", "1.20.1"};
    static const std::vector<std::string> kBedrock = {
        "1.21.50", "1.21.40", "1.21.30", "1.21.20", "1.21.10", "1.21.0",
        "1.20.80", "1.20.70", "1.20.60"};
    switch (l) {
        case Loader::Forge: return kForge;
        case Loader::NeoForge: return kNeo;
        case Loader::Bedrock: return kBedrock;
        case Loader::Fabric: break;
    }
    return kFabric;
}

// --- Normalisations ---------------------------------------------------------

std::string mod_id_from(const std::string& name) {
    const std::string flat = deaccent(name);
    std::string out;
    for (char c : flat) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u))
            out.push_back(static_cast<char>(std::tolower(u)));
        else if (!out.empty() && out.back() != '_')
            out.push_back('_');
    }
    while (!out.empty() && out.back() == '_') out.pop_back();
    // Fabric exige une premiere lettre alphabetique.
    while (!out.empty() && !std::isalpha(static_cast<unsigned char>(out.front())))
        out.erase(out.begin());
    if (out.size() > 63) out.resize(63);
    return out;
}

std::string class_name_from(const std::string& name) {
    const std::string flat = deaccent(name);
    std::string out;
    bool up = true;
    for (char c : flat) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u)) {
            out.push_back(up ? static_cast<char>(std::toupper(u)) : c);
            up = false;
        } else {
            up = true;
        }
    }
    // Un identifiant Java ne commence pas par un chiffre.
    while (!out.empty() && std::isdigit(static_cast<unsigned char>(out.front())))
        out.erase(out.begin());
    return out.empty() ? "MonMod" : out;
}

bool valid_package(const std::string& pkg) {
    static const std::vector<std::string> kReserved = {
        "abstract", "assert",  "boolean", "break",  "byte",   "case",
        "catch",    "char",    "class",   "const",  "continue", "default",
        "do",       "double",  "else",    "enum",   "extends", "final",
        "finally",  "float",   "for",     "goto",   "if",      "implements",
        "import",   "instanceof", "int",  "interface", "long", "native",
        "new",      "package", "private", "protected", "public", "return",
        "short",    "static",  "strictfp", "super", "switch",  "synchronized",
        "this",     "throw",   "throws",  "transient", "try",  "void",
        "volatile", "while",   "true",    "false",  "null",    "_"};
    if (pkg.empty()) return false;
    std::size_t start = 0;
    while (start <= pkg.size()) {
        const std::size_t dot = pkg.find('.', start);
        const std::string seg =
            pkg.substr(start, dot == std::string::npos ? std::string::npos
                                                       : dot - start);
        if (seg.empty()) return false;
        if (!std::islower(static_cast<unsigned char>(seg[0]))) return false;
        for (char c : seg) {
            const unsigned char u = static_cast<unsigned char>(c);
            if (!(std::islower(u) || std::isdigit(u) || c == '_')) return false;
        }
        if (std::find(kReserved.begin(), kReserved.end(), seg) != kReserved.end())
            return false;
        if (dot == std::string::npos) break;
        start = dot + 1;
    }
    return true;
}

std::string uuid_v4() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<std::uint64_t> d;
    std::uint64_t a = d(rng), b = d(rng);
    // Version 4 et variante RFC 4122.
    a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
    char buf[40];
    std::snprintf(buf, sizeof(buf),
                  "%08x-%04x-%04x-%04x-%012llx",
                  static_cast<unsigned>(a >> 32),
                  static_cast<unsigned>((a >> 16) & 0xFFFF),
                  static_cast<unsigned>(a & 0xFFFF),
                  static_cast<unsigned>(b >> 48),
                  static_cast<unsigned long long>(b & 0xFFFFFFFFFFFFull));
    return buf;
}

// --- Versions des dependances -----------------------------------------------

// fabric-api n'est pas servi par meta.fabricmc.net : ses versions vivent
// dans le maven — celui-la meme ou Gradle ira les chercher. On lit donc
// le meme index que lui, ce qui evite toute divergence entre ce qu'on
// ecrit dans gradle.properties et ce que la construction sait resoudre.
//
// Le suffixe « +<version du jeu> » est ce qui lie une version de
// fabric-api a une version de Minecraft : 0.119.4+1.21.4. Les entrees du
// maven sont dans l'ordre de publication, donc la derniere qui porte le
// bon suffixe est la plus recente.
std::string resolve_fabric_api(const std::string& mcVersion) { // NOLINT
    // (declaree dans moddev.hpp : exposee pour le test reseau)
    const auto body = http::get_string(
        "https://maven.fabricmc.net/net/fabricmc/fabric-api/fabric-api/"
        "maven-metadata.xml");
    if (!body) return {};
    const std::string suffix = "+" + mcVersion;
    std::string best;
    std::size_t p = 0;
    for (;;) {
        const auto a = body->find("<version>", p);
        if (a == std::string::npos) break;
        const auto b = body->find("</version>", a);
        if (b == std::string::npos) break;
        const std::string v = body->substr(a + 9, b - a - 9);
        p = b + 10;
        // Comparaison ancree sur la fin : « +1.21.4 » ne doit pas accepter
        // une version de 1.21.41 si elle existait un jour.
        if (v.size() > suffix.size() &&
            v.compare(v.size() - suffix.size(), suffix.size(), suffix) == 0)
            best = v;
    }
    return best;
}

Deps resolve_deps(Loader l, const std::string& mcVersion) {
    Deps d;
    d.yarn = mcVersion + kFallbackYarnSuffix;
    d.loader = kFallbackLoader;
    d.fabricApi = "";
    d.forge = mcVersion + "-recommended";
    d.neoforge = "";

    if (l != Loader::Fabric) {
        d.note =
            "Versions " + std::string(loader_name(l)) +
            " non resolues en ligne : verifie la version exacte du chargeur "
            "dans gradle.properties avant le premier build.";
        return d;
    }

    // meta.fabricmc.net est deja dans l'allowlist (installation de Fabric).
    bool okYarn = false, okLoader = false;
    if (auto body = http::get_string("https://meta.fabricmc.net/v2/versions/yarn/" +
                                     mcVersion)) {
        const auto j = json::parse(*body, nullptr, false);
        if (j.is_array() && !j.empty() && j[0].is_object()) {
            d.yarn = j[0].value("version", d.yarn);
            okYarn = true;
        }
    }
    if (auto body = http::get_string(
            "https://meta.fabricmc.net/v2/versions/loader/" + mcVersion)) {
        const auto j = json::parse(*body, nullptr, false);
        if (j.is_array() && !j.empty() && j[0].is_object()) {
            const auto it = j[0].find("loader");
            if (it != j[0].end() && it->is_object()) {
                d.loader = it->value("version", d.loader);
                okLoader = true;
            }
        }
    }
    d.fabricApi = resolve_fabric_api(mcVersion);

    d.resolved = okYarn && okLoader;
    if (!d.resolved)
        d.note =
            "Versions Fabric non resolues (hors ligne ou service indisponible) : "
            "valeurs de repli ecrites dans gradle.properties, a verifier sur "
            "fabricmc.net/develop avant le premier build.";
    else if (d.fabricApi.empty())
        d.note =
            "fabric-api : aucune version publiee pour Minecraft " + mcVersion +
            " (ou maven.fabricmc.net injoignable). La dependance est omise — "
            "le squelette compile et se lance sans elle, puisqu'il n'utilise "
            "que le chargeur. Ajoute-la quand tu en auras besoin.";
    return d;
}

// --- Generation -------------------------------------------------------------

namespace {

// `spec` n est pas utilise : tout passe par gradle.properties.
std::string fabric_gradle(const Deps& d, const std::string& archive) {
    std::ostringstream o;
    o << "plugins {\n"
      << "    id 'fabric-loom' version '1.9-SNAPSHOT'\n"
      << "    id 'maven-publish'\n"
      << "}\n\n"
      << "version = project.mod_version\n"
      << "group = project.maven_group\n\n"
      << "base {\n    archivesName = '" << archive << "'\n}\n\n"
      << "repositories {\n    mavenCentral()\n}\n\n"
      << "dependencies {\n"
      << "    minecraft \"com.mojang:minecraft:${project.minecraft_version}\"\n"
      << "    mappings \"net.fabricmc:yarn:${project.yarn_mappings}:v2\"\n"
      << "    modImplementation \"net.fabricmc:fabric-loader:${project.loader_version}\"\n";
    // Omise faute de version reelle. L'ancien repli « [1.0,) » etait une
    // plage Maven inventee, jamais verifiee, et qui ne pouvait rien
    // matcher : TOUTES les versions de fabric-api sont en 0.x. Resultat,
    // le premier build de n'importe qui echouait apres quarante secondes
    // de resolution, sur une liste de 1156 versions « disponibles ».
    if (!d.fabricApi.empty())
        o << "    modImplementation \"net.fabricmc.fabric-api:fabric-api:"
             "${project.fabric_version}\"\n";
    o << "}\n\n"
      << "processResources {\n"
      << "    inputs.property 'version', project.version\n"
      << "    filesMatching('fabric.mod.json') {\n"
      << "        expand 'version': project.version\n"
      << "    }\n"
      << "}\n\n"
      << "java {\n"
      << "    withSourcesJar()\n"
      << "    sourceCompatibility = JavaVersion.VERSION_21\n"
      << "    targetCompatibility = JavaVersion.VERSION_21\n"
      << "}\n";
    (void)d;
    return o.str();
}

std::string forge_gradle(const ProjectSpec& s, bool neo,
                         const std::string& archive) {
    std::ostringstream o;
    if (neo) {
        o << "plugins {\n    id 'net.neoforged.moddev' version '2.0.78'\n}\n\n";
        o << "version = project.mod_version\ngroup = project.maven_group\n\n";
        o << "base {\n    archivesName = '" << archive << "'\n}\n\n";
        o << "neoForge {\n"
          << "    version = project.neoforge_version\n"
          << "    runs {\n        client { client() }\n        server { server() }\n    }\n"
          << "    mods {\n        \"" << mod_id_from(s.name) << "\" {\n"
          << "            sourceSet sourceSets.main\n        }\n    }\n}\n\n";
    } else {
        o << "plugins {\n    id 'net.minecraftforge.gradle' version '[6.0,6.2)'\n}\n\n";
        o << "version = project.mod_version\ngroup = project.maven_group\n\n";
        o << "base {\n    archivesName = '" << archive << "'\n}\n\n";
        o << "minecraft {\n"
          << "    mappings channel: 'official', version: project.minecraft_version\n"
          << "}\n\n"
          << "dependencies {\n"
          << "    minecraft \"net.minecraftforge:forge:${project.forge_version}\"\n"
          << "}\n\n";
    }
    o << "java {\n    sourceCompatibility = JavaVersion.VERSION_21\n"
      << "    targetCompatibility = JavaVersion.VERSION_21\n}\n";
    return o.str();
}

std::string properties_for(const ProjectSpec& s, const Deps& d) {
    std::ostringstream o;
    o << "org.gradle.jvmargs=-Xmx2G\n"
      << "org.gradle.parallel=true\n\n"
      << "minecraft_version=" << s.mcVersion << "\n"
      << "mod_version=1.0.0\n"
      << "maven_group=" << s.pkg << "\n";
    switch (s.loader) {
        case Loader::Fabric:
            o << "yarn_mappings=" << d.yarn << "\n"
              << "loader_version=" << d.loader << "\n";
            if (!d.fabricApi.empty())
                o << "fabric_version=" << d.fabricApi << "\n";
            break;
        case Loader::Forge:
            o << "forge_version=" << d.forge << "\n";
            break;
        case Loader::NeoForge:
            o << "neoforge_version=" << d.neoforge << "\n";
            break;
        case Loader::Bedrock:
            break;
    }
    if (!d.resolved && !d.note.empty()) o << "\n# " << d.note << "\n";
    return o.str();
}

std::string java_source(const ProjectSpec& s, const std::string& cls,
                        const std::string& modId) {
    std::ostringstream o;
    o << "package " << s.pkg << ";\n\n";
    if (s.loader == Loader::Fabric) {
        o << "import net.fabricmc.api.ModInitializer;\n"
          << "import org.slf4j.Logger;\n"
          << "import org.slf4j.LoggerFactory;\n\n"
          << "public class " << cls << " implements ModInitializer {\n"
          << "    public static final String MOD_ID = \"" << modId << "\";\n"
          << "    public static final Logger LOGGER = LoggerFactory.getLogger(MOD_ID);\n\n"
          << "    @Override\n    public void onInitialize() {\n"
          << "        LOGGER.info(\"{} charge !\", MOD_ID);\n    }\n}\n";
    } else {
        const char* ann = s.loader == Loader::NeoForge
                              ? "net.neoforged.fml.common.Mod"
                              : "net.minecraftforge.fml.common.Mod";
        o << "import " << ann << ";\n"
          << "import org.slf4j.Logger;\n"
          << "import com.mojang.logging.LogUtils;\n\n"
          << "@Mod(" << cls << ".MOD_ID)\n"
          << "public class " << cls << " {\n"
          << "    public static final String MOD_ID = \"" << modId << "\";\n"
          << "    private static final Logger LOGGER = LogUtils.getLogger();\n\n"
          << "    public " << cls << "() {\n"
          << "        LOGGER.info(\"{} charge !\", MOD_ID);\n    }\n}\n";
    }
    return o.str();
}

} // namespace

std::map<std::string, std::string> project_files(const ProjectSpec& spec,
                                                 const Deps& deps) {
    std::map<std::string, std::string> out;
    const std::string modId = mod_id_from(spec.name);
    const std::string cls = class_name_from(spec.name);

    if (spec.loader == Loader::Bedrock) {
        const std::string hdr = uuid_v4(), bpMod = uuid_v4();
        const std::string rhdr = uuid_v4(), rpMod = uuid_v4();
        json bp = {
            {"format_version", 2},
            {"header",
             {{"name", spec.name},
              {"description", "Add-on genere par Team Launcher"},
              {"uuid", hdr},
              {"version", json::array({1, 0, 0})},
              {"min_engine_version", json::array({1, 21, 0})}}},
            {"modules", json::array({json{{"type", "data"},
                                          {"uuid", bpMod},
                                          {"version", json::array({1, 0, 0})}}})}};
        json rp = {
            {"format_version", 2},
            {"header",
             {{"name", spec.name + " Resources"},
              {"description", "Pack de ressources"},
              {"uuid", rhdr},
              {"version", json::array({1, 0, 0})},
              {"min_engine_version", json::array({1, 21, 0})}}},
            {"modules", json::array({json{{"type", "resources"},
                                          {"uuid", rpMod},
                                          {"version", json::array({1, 0, 0})}}})}};
        out["BP/manifest.json"] = bp.dump(2);
        out["RP/manifest.json"] = rp.dump(2);
        // Le C# creait des dossiers vides, que rien ne conserve dans une
        // archive ni dans Git. Un .gitkeep les rend reels.
        for (const char* d : {"BP/entities", "BP/blocks", "BP/items",
                              "BP/scripts", "RP/textures", "RP/models"})
            out[std::string(d) + "/.gitkeep"] = "";
        out["README.md"] =
            "# " + spec.name +
            "\n\nAdd-on Bedrock.\n\n- `BP/` : behavior pack\n- `RP/` : resource "
            "pack\n\nCopie les deux dossiers dans "
#ifdef _WIN32
            "`%LOCALAPPDATA%\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\"
            "LocalState\\games\\com.mojang\\development_*_packs\\`.\n"
#else
            // Pas de Bedrock UWP sous Linux : chemin generique, pas invente.
            "le dossier `development_*_packs` de ton installation Bedrock "
            "(`games/com.mojang/`).\n"
#endif
            "Les modeles 3D se font avec Blockbench.\n";
        return out;
    }

    const std::string archive = modId.empty() ? "mod" : modId;
    out["settings.gradle"] =
        std::string("pluginManagement {\n    repositories {\n") +
        (spec.loader == Loader::Fabric
             ? "        maven { name = 'Fabric'; url = 'https://maven.fabricmc.net/' }\n"
             : spec.loader == Loader::NeoForge
                   ? "        maven { url = 'https://maven.neoforged.net/releases' }\n"
                   : "        maven { url = 'https://maven.minecraftforge.net/' }\n") +
        "        mavenCentral()\n        gradlePluginPortal()\n    }\n}\n\n"
        "rootProject.name = '" + archive + "'\n";

    out["build.gradle"] =
        spec.loader == Loader::Fabric
            ? fabric_gradle(deps, archive)
            : forge_gradle(spec, spec.loader == Loader::NeoForge, archive);
    out["gradle.properties"] = properties_for(spec, deps);
    out[".gitignore"] =
        "build/\n.gradle/\nrun/\n.idea/\n*.iml\n.vscode/\n";

    std::string pkgPath = spec.pkg;
    std::replace(pkgPath.begin(), pkgPath.end(), '.', '/');
    out["src/main/java/" + pkgPath + "/" + cls + ".java"] =
        java_source(spec, cls, modId);

    if (spec.loader == Loader::Fabric) {
        // Le palier Java suit la version du jeu, il ne se devine pas :
        // « >=21 » en dur aurait exige un JDK 21 d'un mod 1.18, qui tourne
        // sur 17.
        json depends = {
            {"fabricloader", ">=" + deps.loader},
            {"minecraft", "~" + spec.mcVersion},
            {"java", ">=" + std::to_string(jdk_major_for(spec.mcVersion))}};
        // Ne declarer fabric-api que si le projet en depend reellement :
        // sinon le mod refuse de se charger faute d'une bibliotheque qu'il
        // n'utilise meme pas.
        if (!deps.fabricApi.empty()) depends["fabric-api"] = "*";
        json f = {{"schemaVersion", 1},
                  {"id", modId},
                  {"version", "${version}"},
                  {"name", spec.name},
                  {"environment", "*"},
                  {"entrypoints", {{"main", json::array({spec.pkg + "." + cls})}}},
                  {"depends", depends}};
        out["src/main/resources/fabric.mod.json"] = f.dump(2);
    } else {
        std::ostringstream toml;
        toml << "modLoader=\""
             << (spec.loader == Loader::NeoForge ? "javafml" : "javafml")
             << "\"\nloaderVersion=\"[1,)\"\nlicense=\"MIT\"\n\n"
             << "[[mods]]\nmodId=\"" << modId << "\"\nversion=\"1.0.0\"\n"
             << "displayName=\"" << spec.name << "\"\n"
             << "description='''Mod genere par Team Launcher.'''\n";
        out[spec.loader == Loader::NeoForge
                ? "src/main/resources/META-INF/neoforge.mods.toml"
                : "src/main/resources/META-INF/mods.toml"] = toml.str();
    }

    out["README.md"] =
        "# " + spec.name + "\n\nMod " + loader_name(spec.loader) + " pour " +
        "Minecraft " + spec.mcVersion +
        ".\n\n## Construire\n\n```\ngradle build\n```\n\n"
        "Le launcher utilise le wrapper `gradlew` du projet s'il existe, sinon "
        "le `gradle` du PATH. Pour figer une version de Gradle :\n\n"
        "```\ngradle wrapper --gradle-version 8.12\n```\n";
    return out;
}

CreateResult create_project(const ProjectSpec& spec, const Deps& deps,
                            bool overwrite, const Log& log) {
    CreateResult r;
    auto say = [&](const std::string& m) { if (log) log(m); };

    if (spec.name.empty()) {
        r.error = "Donne un nom au mod.";
        return r;
    }
    if (spec.dir.empty()) {
        r.error = "Choisis un dossier de projet.";
        return r;
    }
    if (spec.loader != Loader::Bedrock && !valid_package(spec.pkg)) {
        r.error = "Nom de paquet Java invalide : " + spec.pkg +
                  " (minuscules, chiffres et points, ex. com.exemple.monmod).";
        return r;
    }
    if (spec.loader != Loader::Bedrock && mod_id_from(spec.name).empty()) {
        r.error = "Le nom du mod ne donne aucun identifiant exploitable.";
        return r;
    }

    std::error_code ec;
    fs::create_directories(spec.dir, ec);
    if (ec) {
        r.error = "Creation du dossier impossible : " + ec.message();
        return r;
    }

    const auto files = project_files(spec, deps);
    for (const auto& [rel, content] : files) {
        const fs::path p = spec.dir / rel;
        if (!overwrite && file_exists(p)) {
            r.kept.push_back(rel);
            continue;
        }
        fs::create_directories(p.parent_path(), ec);
        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out) {
            r.error = "Ecriture impossible : " + rel;
            return r;
        }
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        r.written.push_back(rel);
    }

    say("Projet " + std::string(loader_name(spec.loader)) + " " + spec.mcVersion +
        " : " + std::to_string(r.written.size()) + " fichier(s) ecrit(s)" +
        (r.kept.empty() ? ""
                        : ", " + std::to_string(r.kept.size()) +
                              " deja present(s) et conserve(s)"));
    if (!deps.note.empty()) say(deps.note);
    r.ok = true;
    return r;
}

// --- Chaine d'outils --------------------------------------------------------

int jdk_major_for(const std::string& mcVersion) {
    // Memes paliers que le jeu lui-meme : 21 depuis 1.20.5, 17 depuis
    // 1.18, 8 avant. On prend 21 par defaut quand on ne sait pas — c'est
    // la valeur juste pour tout ce qui se developpe aujourd'hui, et un
    // JDK 21 compile aussi vers des cibles plus anciennes.
    if (mcVersion.empty()) return 21;
    int maj = 0, min = 0, pat = 0;
    if (std::sscanf(mcVersion.c_str(), "%d.%d.%d", &maj, &min, &pat) < 2)
        return 21;
    if (maj != 1) return 21;
    if (min > 20 || (min == 20 && pat >= 5)) return 21;
    if (min >= 18) return 17;
    return 8;
}

std::string managed_gradle_path() {
    std::error_code ec;
    const fs::path root = runtime_root() / "gradle";
    if (!fs::is_directory(root, ec)) return {};
    // On ne fige pas le numero de version ici : une installation faite
    // par une version precedente du launcher doit rester utilisable.
    for (const auto& e : fs::directory_iterator(root, ec)) {
        if (ec) break;
        if (!e.is_directory(ec)) continue;
#ifdef _WIN32
        const fs::path exe = e.path() / "bin" / "gradle.bat";
#else
        const fs::path exe = e.path() / "bin" / "gradle";
#endif
        if (fs::is_regular_file(exe, ec)) return exe.string();
    }
    return {};
}

std::string ensure_gradle(const Log& log, const std::atomic<bool>* cancel,
                          std::string* errOut) {
    auto say = [&](const std::string& m) {
        if (log) log(m);
    };
    if (const std::string have = managed_gradle_path(); !have.empty()) {
        say("Gradle déjà installé : " + have);
        return have;
    }

    constexpr const char* kVersion = "8.12";
    const fs::path root = runtime_root() / "gradle";
    std::error_code ec;
    fs::create_directories(root, ec);
    const fs::path zipPath = root / (std::string("gradle-") + kVersion + "-bin.zip");

    say(std::string("Téléchargement de Gradle ") + kVersion + " (~130 Mo)...");
    const std::string url = std::string("https://services.gradle.org/distributions/"
                                        "gradle-") + kVersion + "-bin.zip";
    if (!http::get_to_file(url, zipPath, nullptr, cancel)) {
        fs::remove(zipPath, ec);
        if (errOut) *errOut = "Téléchargement de Gradle impossible (réseau ?).";
        return {};
    }
    if (cancel && cancel->load()) {
        fs::remove(zipPath, ec);
        if (errOut) *errOut = "Annulé.";
        return {};
    }

    say("Extraction...");
    if (zip_extract_all(zipPath, root) < 0) {
        fs::remove(zipPath, ec);
        if (errOut) *errOut = "Archive Gradle illisible.";
        return {};
    }
    // L'archive pèse 130 Mo : la garder après extraction doublerait la
    // place occupée pour rien.
    fs::remove(zipPath, ec);

    const std::string exe = managed_gradle_path();
    if (exe.empty()) {
        if (errOut)
            *errOut = "Gradle extrait, mais son exécutable reste introuvable.";
        return {};
    }
#ifndef _WIN32
    // L'extraction zip ne conserve pas le bit exécutable.
    fs::permissions(exe,
                    fs::perms::owner_exec | fs::perms::group_exec |
                        fs::perms::others_exec,
                    fs::perm_options::add, ec);
#endif
    say("Gradle installé : " + exe);
    return exe;
}

Toolchain detect_toolchain(const fs::path& projectDir,
                           const std::string& mcVersion) {
    Toolchain t;
    t.javaNeeded = jdk_major_for(mcVersion);
#ifdef _WIN32
    const fs::path wrapper = projectDir / "gradlew.bat";
    t.wrapper = file_exists(wrapper);

    const fs::path g = which(L"gradle.bat").empty() ? which(L"gradle.exe")
                                                    : which(L"gradle.bat");
    t.gradleOnPath = !g.empty();

    // On cherche JAVAC, pas java : un JRE lance le jeu mais ne compile
    // rien, et annoncer « JDK oui » a quelqu'un qui n'a qu'un JRE le
    // laisse buter sur une erreur de Gradle incomprehensible.
    fs::path javacExe, javaExe;
    if (const char* jh = std::getenv("JAVA_HOME")) {
        const fs::path c = fs::path(jh) / "bin" / "javac.exe";
        if (file_exists(c)) javacExe = c;
    }
    if (javacExe.empty()) javacExe = which(L"javac.exe");
    javaExe = javacExe.empty() ? which(L"java.exe")
                               : javacExe.parent_path() / "java.exe";
#else
    const fs::path wrapper = projectDir / "gradlew";
    t.wrapper = file_exists(wrapper);

    const fs::path g = which("gradle");
    t.gradleOnPath = !g.empty();

    fs::path javacExe, javaExe;
    if (const char* jh = std::getenv("JAVA_HOME")) {
        const fs::path c = fs::path(jh) / "bin" / "javac";
        std::error_code ecj;
        if (fs::is_regular_file(c, ecj) && ::access(c.string().c_str(), X_OK) == 0)
            javacExe = c;
    }
    if (javacExe.empty()) javacExe = which("javac");
    javaExe = javacExe.empty() ? which("java")
                               : javacExe.parent_path() / "java";
#endif
    t.jdk = !javacExe.empty();
    t.javaPresent = t.jdk || !javaExe.empty();
    if (!javacExe.empty()) t.javaHome = javacExe.parent_path().parent_path().string();
    if (!javaExe.empty()) t.javaMajor = detect_java_major(javaExe.string());

    // Un JDK gere par le launcher compte comme un JDK, meme absent du
    // PATH — et il l'emporte sur un JDK du PATH TROP ANCIEN. Sans cette
    // seconde condition, un JDK 8 installe sur la machine masquait le
    // JDK 21 que le launcher venait de telecharger, et la page continuait
    // de reclamer ce qu'elle avait deja.
    if (!t.jdk || (t.javaMajor > 0 && t.javaMajor < t.javaNeeded)) {
        std::error_code ecm;
        const fs::path managed =
            runtime_root() / ("jdk-" + std::to_string(t.javaNeeded));
#ifdef _WIN32
        const char* javacName = "javac.exe";
#else
        const char* javacName = "javac";
#endif
        if (fs::is_directory(managed, ecm))
            for (auto it = fs::recursive_directory_iterator(
                     managed, fs::directory_options::skip_permission_denied, ecm);
                 it != fs::recursive_directory_iterator(); it.increment(ecm)) {
                if (ecm) break;
                if (it->path().filename() != javacName) continue;
                t.jdk = true;
                t.javaPresent = true;
                t.javaHome = it->path().parent_path().parent_path().string();
                t.javaMajor = t.javaNeeded;
                break;
            }
    }

    // Gradle gere par le launcher : dernier recours, avant de declarer
    // qu'il manque quelque chose.
    const std::string managedGradle = managed_gradle_path();
    t.gradleManaged = !managedGradle.empty();

    if (t.wrapper)
        t.command = "\"" + wrapper.string() + "\"";
    else if (t.gradleOnPath)
        t.command = "\"" + g.string() + "\"";
    else if (t.gradleManaged)
        t.command = "\"" + managedGradle + "\"";

    // Dire a Gradle QUEL Java utiliser. Sans cela il prend celui du PATH :
    // sur une machine ou traine un vieux JDK 8, la construction echoue
    // alors que le bon JDK vient d'etre telecharge et se trouve juste a
    // cote. `-Dorg.gradle.java.home` existe exactement pour ca, et evite
    // d'avoir a bricoler l'environnement du processus fils.
    if (!t.command.empty() && !t.javaHome.empty())
        t.command += " \"-Dorg.gradle.java.home=" + t.javaHome + "\"";

    // Les messages disent ce que le LAUNCHER peut faire, pas ce que
    // l'utilisateur devrait aller installer lui-meme. L'ancien conseil —
    // « genere le wrapper : gradle wrapper ... » — etait circulaire :
    // il fallait deja Gradle pour l'executer.
    //
    // Et on ENUMERE ce qui manque au lieu de s'arreter au premier :
    // signaler Gradle, puis — une fois Gradle installe — decouvrir que le
    // JDK ne convient pas, fait vivre deux fois le meme echec.
    std::vector<std::string> missing;
    if (t.command.empty()) missing.push_back("Gradle");
    if (!t.jdk) {
        missing.push_back(t.javaPresent
                              ? "un JDK " + std::to_string(t.javaNeeded) +
                                    " (le Java présent est un JRE : il lance "
                                    "le jeu, il ne compile pas)"
                              : "un JDK " + std::to_string(t.javaNeeded));
    } else if (t.javaMajor > 0 && t.javaMajor < t.javaNeeded) {
        // Cas le plus traitre : tout semble present, et la construction
        // echoue sur une erreur de Gradle qui ne nomme pas le coupable.
        missing.push_back("un JDK " + std::to_string(t.javaNeeded) +
                          " (le JDK " + std::to_string(t.javaMajor) +
                          " installé est trop ancien pour " +
                          (mcVersion.empty() ? std::string("cette version")
                                             : "Minecraft " + mcVersion) +
                          ")");
    }

    if (!missing.empty()) {
        t.problem = "Il manque ";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i) t.problem += i + 1 == missing.size() ? " et " : ", ";
            t.problem += missing[i];
        }
        t.problem += missing.size() > 1
                         ? ". « Installer la chaîne d'outils » les télécharge "
                           "tous les deux ; rien à installer à la main."
                         : ". « Installer la chaîne d'outils » le télécharge ; "
                           "rien à installer à la main.";
    }
    return t;
}

// --- Execution --------------------------------------------------------------

int run(const fs::path& dir, const std::string& commandLine, const Log& log,
        const std::atomic<bool>* cancel) {
    auto say = [&](const std::string& m) { if (log) log(m); };
    if (commandLine.empty()) {
        say("Aucune commande a executer.");
        return -1;
    }

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) {
        say("Creation du tube impossible.");
        return -1;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;   // stdout et stderr fusionnes : l'ordre est conserve
    si.hStdInput = nullptr;

    std::wstring cmd = widen(commandLine);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const std::wstring wdir = dir.wstring();

    PROCESS_INFORMATION pi{};
    const BOOL ok =
        CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr,
                       wdir.empty() ? nullptr : wdir.c_str(), &si, &pi);
    CloseHandle(wr);
    if (!ok) {
        CloseHandle(rd);
        say("Lancement impossible (code " + std::to_string(GetLastError()) +
            ") : " + commandLine);
        return -1;
    }
    CloseHandle(pi.hThread);

    // Lecture ligne a ligne : la console doit avancer pendant le build, pas
    // se remplir d'un coup a la fin.
    std::string pending;
    char raw[4096];
    DWORD read = 0;
    while (ReadFile(rd, raw, sizeof(raw), &read, nullptr) && read > 0) {
        pending.append(raw, read);
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            say(line);
            pending.erase(0, nl + 1);
        }
        if (cancel && cancel->load()) {
            TerminateProcess(pi.hProcess, 1);
            say("Interrompu.");
            break;
        }
    }
    if (!pending.empty()) say(pending);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
#else
    // sh -c : l'equivalent de la ligne brute CreateProcess (qui parse aussi).
    // stdout+stderr fusionnes et pompe ligne a ligne, comme cote Windows.
    int spawnErr = 0;
    auto child = proc::spawn_shell(commandLine, dir.string(), &spawnErr);
    if (!child) {
        say("Lancement impossible (code " + std::to_string(spawnErr) + ") : " +
            commandLine);
        return -1;
    }
    std::string pending;
    char raw[4096];
    for (;;) {
        const ssize_t n = ::read(child->outFd, raw, sizeof(raw));
        if (n <= 0) break;
        pending.append(raw, static_cast<size_t>(n));
        std::size_t nl;
        while ((nl = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, nl);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            say(line);
            pending.erase(0, nl + 1);
        }
        if (cancel && cancel->load()) {
            proc::terminate_child(child->pid);
            say("Interrompu.");
            break;
        }
    }
    if (!pending.empty()) say(pending);
    const int code = proc::wait_exit(child->pid, -1);
    proc::close_fd(child->outFd);
    return code;
#endif
}

} // namespace tl::moddev

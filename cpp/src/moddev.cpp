#include "moddev.hpp"

#include "http_win.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>
#include <thread>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::moddev {

namespace {

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(),
                                      static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                        w.data(), n);
    return w;
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
fs::path which(const std::wstring& exe) {
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, exe.c_str(), nullptr, MAX_PATH, buf, nullptr) > 0)
        return fs::path(buf);
    return {};
}

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
    if (_stricmp(s.c_str(), "forge") == 0) return Loader::Forge;
    if (_stricmp(s.c_str(), "neoforge") == 0) return Loader::NeoForge;
    if (_stricmp(s.c_str(), "bedrock") == 0) return Loader::Bedrock;
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
    d.resolved = okYarn && okLoader;
    if (!d.resolved)
        d.note =
            "Versions Fabric non resolues (hors ligne ou service indisponible) : "
            "valeurs de repli ecrites dans gradle.properties, a verifier sur "
            "fabricmc.net/develop avant le premier build.";
    if (d.fabricApi.empty())
        d.fabricApi = "[1.0,)"; // plage Maven : laisse Gradle choisir
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
      << "    modImplementation \"net.fabricmc:fabric-loader:${project.loader_version}\"\n"
      << "    modImplementation \"net.fabricmc.fabric-api:fabric-api:${project.fabric_version}\"\n"
      << "}\n\n"
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
              << "loader_version=" << d.loader << "\n"
              << "fabric_version=" << d.fabricApi << "\n";
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
            "`%LOCALAPPDATA%\\Packages\\Microsoft.MinecraftUWP_8wekyb3d8bbwe\\"
            "LocalState\\games\\com.mojang\\development_*_packs\\`.\n"
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
        json f = {{"schemaVersion", 1},
                  {"id", modId},
                  {"version", "${version}"},
                  {"name", spec.name},
                  {"environment", "*"},
                  {"entrypoints", {{"main", json::array({spec.pkg + "." + cls})}}},
                  {"depends",
                   {{"fabricloader", ">=" + deps.loader},
                    {"fabric-api", "*"},
                    {"minecraft", "~" + spec.mcVersion},
                    {"java", ">=21"}}}};
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

Toolchain detect_toolchain(const fs::path& projectDir) {
    Toolchain t;
    const fs::path wrapper = projectDir / "gradlew.bat";
    t.wrapper = file_exists(wrapper);

    const fs::path g = which(L"gradle.bat").empty() ? which(L"gradle.exe")
                                                    : which(L"gradle.bat");
    t.gradleOnPath = !g.empty();

    // JDK : JAVA_HOME d'abord, puis le PATH.
    fs::path javaExe;
    if (const char* jh = std::getenv("JAVA_HOME")) {
        const fs::path c = fs::path(jh) / "bin" / "java.exe";
        if (file_exists(c)) javaExe = c;
    }
    if (javaExe.empty()) javaExe = which(L"java.exe");
    t.jdk = !javaExe.empty();

    if (t.wrapper)
        t.command = "\"" + wrapper.string() + "\"";
    else if (t.gradleOnPath)
        t.command = "\"" + g.string() + "\"";

    if (!t.jdk) {
        t.problem =
            "Aucun JDK trouve (ni JAVA_HOME, ni java dans le PATH). Installe "
            "un JDK 21 : Gradle ne demarrera pas sans.";
    } else if (t.command.empty()) {
        // CORRECTIF : le C# disait « cree le projet d'abord », alors que
        // creer le projet ne produisait aucun wrapper.
        t.problem =
            "Ni wrapper Gradle dans le projet (gradlew.bat), ni gradle dans le "
            "PATH. Installe Gradle, ou genere le wrapper une fois depuis le "
            "dossier du projet : gradle wrapper --gradle-version 8.12";
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
}

} // namespace tl::moddev

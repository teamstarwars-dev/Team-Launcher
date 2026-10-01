#include "crash_analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace tl::crash {

namespace {

// ---------------------------------------------------------------------------
// Règles de cause
// ---------------------------------------------------------------------------

struct Rule {
    Cause cause;
    const char* const* needles; // terminé par nullptr
    const char* title;
    const char* action;
};

const char* const kOom[] = {"OutOfMemoryError", nullptr};
const char* const kJavaVer[] = {"UnsupportedClassVersionError", nullptr};
const char* const kMissingClass[] = {"NoClassDefFoundError",
                                     "ClassNotFoundException", nullptr};
const char* const kSession[] = {"Invalid session",
                                "Failed to verify authentication", nullptr};
// « OpenGL » tout court était ici. C'était un défaut sérieux : la section
// « System Details » de TOUT rapport de crash contient le mot (version du
// pilote, capacités GL). N'importe quel plantage non reconnu plus haut
// était donc diagnostiqué « problème de pilote graphique » — vérifié sur
// un rapport réel dont la vraie cause était un mod. On n'accepte plus que
// des formulations qui ne peuvent venir que d'un véritable échec graphique.
const char* const kGl[] = {"Pixel format not accelerated",
                           "Failed to create window",
                           "GLFW error",
                           "No OpenGL context",
                           "OpenGL 1.1",
                           "WGL_ARB_create_context",
                           "EXCEPTION_ACCESS_VIOLATION",
                           nullptr};
const char* const kHeap[] = {"Could not reserve enough space for object heap",
                             nullptr};
const char* const kDenied[] = {"Access is denied", "AccessDeniedException",
                               nullptr};
const char* const kNet[] = {"UnknownHostException", "Connection timed out",
                            "Connection refused", nullptr};
const char* const kMods[] = {"DuplicateModsFoundException",
                             "ModResolutionException",
                             "Missing or unsupported mandatory dependencies",
                             nullptr};

const Rule kRules[] = {
    {Cause::OutOfMemory, kOom, "Minecraft a manqué de mémoire.",
     "→ Augmente la RAM allouée (Options de lancement de l'instance, ou "
     "Paramètres)."},
    {Cause::JavaVersion, kJavaVer,
     "La version de Java ne correspond pas à cette version de Minecraft.",
     "→ Laisse le launcher télécharger le bon Java, ou installe-le depuis "
     "adoptium.net."},
    {Cause::ModConflict, kMods, "Conflit entre mods (doublon ou dépendance).",
     "→ Retire les doublons du dossier mods de l'instance."},
    {Cause::MissingClass, kMissingClass,
     "Un mod est manquant, corrompu ou incompatible.",
     "→ Retire le dernier mod ajouté, ou répare l'instance."},
    {Cause::Session, kSession, "Ta session de jeu a expiré.",
     "→ Relance simplement : le launcher se reconnecte automatiquement à "
     "Microsoft."},
    {Cause::Heap, kHeap, "Pas assez de RAM libre pour la quantité demandée.",
     "→ Réduis la RAM allouée dans les Options de lancement."},
    {Cause::FileLocked, kDenied, "Un fichier du jeu est bloqué (antivirus ?).",
     "→ Ajoute une exclusion pour le dossier Team Launcher dans ton "
     "antivirus."},
    {Cause::Network, kNet,
     "Problème de connexion Internet pendant le chargement.",
     "→ Vérifie ta connexion et relance."},
    // Le graphique passe EN DERNIER : ses motifs sont les plus susceptibles
    // d'apparaître par accident, et une cause plus précise doit gagner.
    {Cause::Graphics, kGl, "Problème de pilote graphique.",
     "→ Mets à jour les pilotes de ta carte graphique (NVIDIA / AMD / "
     "Intel)."},
};

// ---------------------------------------------------------------------------
// Outils
// ---------------------------------------------------------------------------

std::string lower(std::string s) {
    for (char& c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool contains_ci(const std::string& hay, const char* needle) {
    const size_t n = std::char_traits<char>::length(needle);
    if (n == 0 || hay.size() < n) return false;
    const auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(hay.begin(), hay.end(), needle, needle + n, eq) !=
           hay.end();
}

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && static_cast<unsigned char>(s[a]) <= ' ') ++a;
    while (b > a && static_cast<unsigned char>(s[b - 1]) <= ' ') --b;
    return s.substr(a, b - a);
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : text) {
        if (c == '\n') {
            if (!cur.empty() && cur.back() == '\r') cur.pop_back();
            out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
}

std::vector<std::string> split_pipes(const std::string& line) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : line) {
        if (c == '|') {
            out.push_back(trim(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(trim(cur));
    return out;
}

// Identifiants fournis par la plateforme : jamais des suspects.
bool is_platform(const std::string& idLower) {
    static const std::set<std::string> kSkip = {
        "minecraft", "mcp",   "fml",          "forge",      "neoforge",
        "fabric",    "quilt", "fabricloader", "fabric-api", "java",
        "quilt_loader", "quilt_base", "mixin", "mojang", "oshi",
    };
    return kSkip.count(idLower) != 0;
}

// Un identifiant de deux lettres produit des coïncidences dans n'importe
// quelle trace de pile (« io », « ui », « gl »). Trois, en revanche, est
// une longueur parfaitement courante pour un vrai mod — `jei`, `rei`,
// `ars`, `ic2` — et les écarter revenait à rater les plus installés. Le
// plancher est donc à trois, et c'est la présence dans la liste des mods
// du rapport qui écarte les coïncidences.
bool usable_id(const std::string& id) { return id.size() >= 3; }

} // namespace

const char* cause_key(Cause c) {
    switch (c) {
    case Cause::OutOfMemory: return "out_of_memory";
    case Cause::JavaVersion: return "java_version";
    case Cause::MissingClass: return "missing_class";
    case Cause::Session: return "session_expired";
    case Cause::Graphics: return "graphics_driver";
    case Cause::Heap: return "heap_too_large";
    case Cause::FileLocked: return "file_locked";
    case Cause::Network: return "network";
    case Cause::ModConflict: return "mod_conflict";
    case Cause::ModError: return "mod_error";
    default: return "unknown";
    }
}

// ---------------------------------------------------------------------------
// Liste des mods
// ---------------------------------------------------------------------------

std::vector<ModEntry> parse_mod_list(const std::string& text) {
    std::vector<ModEntry> out;
    std::set<std::string> seen;
    auto add = [&](ModEntry e) {
        e.id = trim(e.id);
        if (e.id.empty()) return;
        const std::string k = lower(e.id);
        if (is_platform(k)) return;
        if (!seen.insert(k).second) return;
        out.push_back(std::move(e));
    };

    const auto lines = split_lines(text);
    bool inFabric = false;
    bool inModList = false;

    for (const auto& raw : lines) {
        const std::string line = trim(raw);

        // --- Fabric : « Fabric Mods: » puis « \t id: Nom version » ---
        if (line.rfind("Fabric Mods:", 0) == 0) {
            inFabric = true;
            inModList = false;
            continue;
        }
        if (inFabric) {
            // La section s'arrête à la première ligne qui n'est pas une
            // entrée indentée.
            if (raw.empty() || (raw[0] != '\t' && raw[0] != ' ')) {
                inFabric = false;
            } else {
                const size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    ModEntry e;
                    e.id = line.substr(0, colon);
                    const std::string rest = trim(line.substr(colon + 1));
                    // « Nom de mod 1.2.3 » : la version est le dernier mot.
                    const size_t sp = rest.rfind(' ');
                    if (sp != std::string::npos) e.version = rest.substr(sp + 1);
                    add(std::move(e));
                }
                continue;
            }
        }

        // --- Forge 1.12 : « | LCH | id | version | source | signature | » ---
        if (line.size() > 4 && line.front() == '|') {
            const auto cols = split_pipes(line);
            // split_pipes rend un champ vide avant le premier « | » et après
            // le dernier : d'où les indices décalés.
            if (cols.size() >= 5) {
                const std::string& state = cols[1];
                // Entête (« State ») et ligne de séparation (« :---- ») :
                // l'état est une suite de lettres majuscules.
                const bool isState =
                    !state.empty() && state.size() <= 5 &&
                    std::all_of(state.begin(), state.end(), [](char c) {
                        return c >= 'A' && c <= 'Z';
                    });
                if (isState) {
                    ModEntry e;
                    e.id = cols[2];
                    e.version = cols.size() > 3 ? cols[3] : "";
                    e.source = cols.size() > 4 ? cols[4] : "";
                    add(std::move(e));
                }
            }
            continue;
        }

        // --- Forge / NeoForge modernes : « Mod List: » puis
        //     « fichier.jar |Nom |id |version |... » ---
        if (line.rfind("Mod List:", 0) == 0) {
            inModList = true;
            continue;
        }
        if (inModList) {
            if (raw.empty() || (raw[0] != '\t' && raw[0] != ' ')) {
                inModList = false;
                continue;
            }
            const auto cols = split_pipes(line);
            if (cols.size() >= 4) {
                ModEntry e;
                e.source = cols[0];
                e.id = cols[2];
                e.version = cols[3];
                add(std::move(e));
            }
            continue;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Trace de pile
// ---------------------------------------------------------------------------

namespace {

// Jetons regroupés PAR CADRE de pile. Le regroupement est ce qui permet de
// noter un suspect correctement : ce qui compte est « à quel cadre le mod
// apparaît », pas « au bout de combien de mots ». Les premiers cadres
// d'une trace sont presque toujours du code de bibliothèque ou de Forge,
// et chacun pèse une dizaine de mots — compter en mots enterrait donc le
// mod fautif sous le bruit de ses appelants.
std::vector<std::vector<std::string>> frame_tokens(const std::string& text) {
    std::vector<std::vector<std::string>> out;
    for (const auto& raw : split_lines(text)) {
        const std::string line = trim(raw);
        const bool frame = line.rfind("at ", 0) == 0;
        const bool exc = line.find("Exception") != std::string::npos ||
                         line.find("Error") != std::string::npos;
        if (!frame && !exc) continue;
        std::vector<std::string> toks;
        std::string cur;
        for (char c : line) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                cur.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            } else {
                if (cur.size() >= 3) toks.push_back(cur);
                cur.clear();
            }
        }
        if (cur.size() >= 3) toks.push_back(cur);
        if (!toks.empty()) out.push_back(std::move(toks));
    }
    return out;
}

} // namespace

std::vector<std::string> stack_tokens(const std::string& text) {
    std::vector<std::string> out;
    for (const auto& raw : split_lines(text)) {
        const std::string line = trim(raw);
        // On ne retient que ce qui ressemble à du code : les cadres de pile
        // et la ligne d'exception. Le reste du rapport (chemins de
        // fichiers, détails système) produirait des coïncidences.
        const bool frame = line.rfind("at ", 0) == 0;
        const bool exc = line.find("Exception") != std::string::npos ||
                         line.find("Error") != std::string::npos;
        if (!frame && !exc) continue;
        // Découpe sur tout ce qui n'est pas alphanumérique : les paquets
        // Java s'écrivent avec des points, les noms de classes internes
        // avec des `/` ou des `$`.
        std::string cur;
        for (char c : line) {
            if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
                cur.push_back(static_cast<char>(
                    std::tolower(static_cast<unsigned char>(c))));
            } else {
                if (cur.size() >= 3) out.push_back(cur);
                cur.clear();
            }
        }
        if (cur.size() >= 3) out.push_back(cur);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Analyse
// ---------------------------------------------------------------------------

namespace {

std::string first_exception(const std::string& text) {
    for (const auto& raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (line.rfind("at ", 0) == 0) continue;
        const size_t ex = line.find("Exception");
        const size_t er = line.find("Error");
        if (ex == std::string::npos && er == std::string::npos) continue;
        // Une ligne d'exception commence par un nom de classe qualifié.
        if (line.find('.') == std::string::npos) continue;
        if (line.size() > 300) continue;
        return line;
    }
    return {};
}

std::string field_after(const std::string& text, const char* label) {
    for (const auto& raw : split_lines(text)) {
        const std::string line = trim(raw);
        if (line.rfind(label, 0) != 0) continue;
        return trim(line.substr(std::char_traits<char>::length(label)));
    }
    return {};
}

} // namespace

Report analyze_report(const std::string& text) {
    Report r;
    if (text.empty()) return r;

    r.exception = first_exception(text);
    r.mcVersion = field_after(text, "Minecraft Version:");

    if (contains_ci(text, "NeoForge")) r.loader = "NeoForge";
    else if (contains_ci(text, "Fabric Mods:") || contains_ci(text, "fabricloader"))
        r.loader = "Fabric";
    else if (contains_ci(text, "Quilt Mods:")) r.loader = "Quilt";
    else if (contains_ci(text, "Powered by Forge") || contains_ci(text, "FML:"))
        r.loader = "Forge";

    // --- 1. Cause générique -------------------------------------------------
    for (const auto& rule : kRules) {
        bool hit = false;
        for (const char* const* n = rule.needles; *n && !hit; ++n)
            hit = contains_ci(text, *n);
        if (!hit) continue;
        r.found = true;
        r.cause = rule.cause;
        r.title = rule.title;
        r.action = rule.action;
        break;
    }

    // --- 2. Désignation du mod ----------------------------------------------
    const auto mods = parse_mod_list(text);
    if (!mods.empty()) {
        const auto frames = frame_tokens(text);
        // Premier CADRE où l'identifiant apparaît : plus c'est haut dans la
        // pile, plus le mod est près de l'erreur.
        std::map<std::string, size_t> firstAt;
        for (size_t i = 0; i < frames.size(); ++i)
            for (const auto& t : frames[i]) firstAt.emplace(t, i);

        for (const auto& m : mods) {
            const std::string idl = lower(m.id);
            if (!usable_id(idl)) continue;
            auto it = firstAt.find(idl);
            if (it == firstAt.end()) continue;
            Suspect s;
            s.modId = m.id;
            s.version = m.version;
            s.source = m.source;
            // 100 au premier cadre, puis 8 de moins par cadre, plancher à
            // 20. La trace se lit de la cause vers l'appelant : le mod cité
            // en haut est celui qui a lancé l'exception.
            const size_t pos = it->second;
            s.score = pos < 10 ? static_cast<int>(100 - pos * 8) : 20;
            // La ligne qui l'accuse, pour qu'on puisse vérifier nous-mêmes.
            for (const auto& raw : split_lines(text)) {
                const std::string line = trim(raw);
                if (line.rfind("at ", 0) != 0) continue;
                if (lower(line).find(idl) == std::string::npos) continue;
                s.evidence = line;
                break;
            }
            r.suspects.push_back(std::move(s));
        }

        // « Suspected Mods: » (Forge moderne) : le jeu lui-même désigne un
        // coupable. On lui fait confiance avant notre propre déduction.
        const std::string suspected = field_after(text, "Suspected Mods:");
        if (!suspected.empty()) {
            const std::string sl = lower(suspected);
            std::set<std::string> boosted;
            for (auto& s : r.suspects)
                if (sl.find(lower(s.modId)) != std::string::npos) {
                    s.score = 100;
                    boosted.insert(lower(s.modId));
                }
            // Le jeu peut désigner un mod qui n'apparaît PAS dans la trace
            // (erreur de chargement, avant toute pile). On le retient quand
            // même : sa parole vaut mieux que notre déduction.
            for (const auto& m : mods) {
                const std::string ml = lower(m.id);
                if (!usable_id(ml) || boosted.count(ml)) continue;
                if (sl.find(ml) == std::string::npos) continue;
                Suspect s;
                s.modId = m.id;
                s.version = m.version;
                s.source = m.source;
                s.evidence = "Suspected Mods: " + suspected;
                s.score = 100;
                r.suspects.push_back(std::move(s));
            }
        }

        std::sort(r.suspects.begin(), r.suspects.end(),
                  [](const Suspect& a, const Suspect& b) {
                      return a.score > b.score;
                  });
        // Au-delà de trois noms, ce n'est plus une désignation.
        if (r.suspects.size() > 3) r.suspects.resize(3);
    }

    if (!r.suspects.empty()) {
        r.found = true;
        // Un mod nommé explique mieux qu'une catégorie — sauf quand la
        // cause générique ne vient pas des mods du tout (mémoire, pilote,
        // session) : là, le mod cité dans la pile n'est pas fautif.
        const bool causeIsEnvironment =
            r.cause == Cause::OutOfMemory || r.cause == Cause::Heap ||
            r.cause == Cause::Session || r.cause == Cause::Network ||
            r.cause == Cause::Graphics || r.cause == Cause::FileLocked;
        if (!causeIsEnvironment) {
            r.cause = r.cause == Cause::Unknown ? Cause::ModError : r.cause;
            if (r.title.empty())
                r.title = "Le crash vient du mod « " + r.suspects[0].modId + " ».";
            if (r.action.empty())
                r.action = "→ Désactive ce mod pour confirmer, puis cherche "
                           "une version compatible.";
        } else {
            // Les suspects restent rapportés (ils peuvent aider) mais ils
            // ne prennent pas la tête du diagnostic.
            r.suspects.clear();
        }
    }

    if (!r.found) return r;

    // --- 3. Résumé lisible --------------------------------------------------
    std::ostringstream o;
    o << r.title << "\n" << r.action;
    if (!r.suspects.empty()) {
        const auto& s = r.suspects[0];
        o << "\n\nMod mis en cause : " << s.modId;
        if (!s.version.empty()) o << " " << s.version;
        if (!s.source.empty()) o << "  (" << s.source << ")";
        if (!s.evidence.empty()) o << "\n" << s.evidence;
        if (r.suspects.size() > 1) {
            o << "\nAutres pistes :";
            for (size_t i = 1; i < r.suspects.size(); ++i)
                o << " " << r.suspects[i].modId;
        }
    }
    r.summary = o.str();
    return r;
}

std::optional<std::string> analyze(const std::string& logText) {
    const Report r = analyze_report(logText);
    if (!r.found) return std::nullopt;
    return r.summary;
}

std::string tail_lines(const fs::path& file, int n) {
    std::ifstream in(file);
    if (!in) return {};
    std::deque<std::string> keep;
    std::string line;
    while (std::getline(in, line)) {
        keep.push_back(line);
        if (static_cast<int>(keep.size()) > n) keep.pop_front();
    }
    std::string out;
    for (size_t i = 0; i < keep.size(); ++i) {
        if (i) out.push_back('\n');
        out += keep[i];
    }
    return out;
}

namespace {

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

Report analyze_instance_report(const fs::path& gameDir) {
    std::error_code ec;

    // 1. rapport de crash officiel (le plus parlant), s'il date de < 5 min.
    // C'est le seul qui porte la liste des mods : le journal du jeu, lui,
    // ne permet pas de nommer un coupable.
    const fs::path crashes = gameDir / "crash-reports";
    if (fs::is_directory(crashes, ec)) {
        fs::path newest;
        fs::file_time_type newestTime{};
        for (const auto& e : fs::directory_iterator(crashes, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() != ".txt") continue;
            const auto t = fs::last_write_time(e.path(), ec);
            if (ec) continue;
            if (newest.empty() || t > newestTime) {
                newest = e.path();
                newestTime = t;
            }
        }
        if (!newest.empty()) {
            const auto age = fs::file_time_type::clock::now() - newestTime;
            if (age < std::chrono::minutes(5)) {
                Report r = analyze_report(read_all(newest));
                if (!r.found) {
                    r.found = true;
                    r.title = "Minecraft a planté (rapport : crash-reports).";
                    r.summary = r.title;
                }
                return r;
            }
        }
    }

    // 2. fin du journal du jeu
    const fs::path gameLog = gameDir / "game-log.txt";
    if (fs::exists(gameLog, ec)) return analyze_report(tail_lines(gameLog, 300));

    return Report{};
}

std::optional<std::string> analyze_instance(const fs::path& gameDir) {
    const Report r = analyze_instance_report(gameDir);
    if (!r.found) return std::nullopt;
    return r.summary;
}

} // namespace tl::crash

#include "crash_analyzer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <deque>
#include <fstream>
#include <sstream>
#include <vector>

namespace fs = std::filesystem;

namespace tl::crash {

namespace {

// Les « regex » du C# ne contiennent que des alternatives litterales (aucun
// metacaractere hors `|`) : une recherche de sous-chaine insensible a la casse
// est strictement equivalente, sans embarquer <regex>.
struct Rule {
    const char* const* needles; // termine par nullptr
    const char* advice;
};

const char* const kOom[] = {"OutOfMemoryError", nullptr};
const char* const kJavaVer[] = {"UnsupportedClassVersionError", nullptr};
const char* const kMissingClass[] = {"NoClassDefFoundError", "ClassNotFoundException",
                                     nullptr};
const char* const kSession[] = {"Invalid session", "Failed to verify authentication",
                                nullptr};
const char* const kGl[] = {"Pixel format not accelerated", "OpenGL", nullptr};
const char* const kHeap[] = {"Could not reserve enough space for object heap",
                             nullptr};
const char* const kDenied[] = {"Access is denied", "AccessDeniedException", nullptr};
const char* const kNet[] = {"UnknownHostException", "Connection timed out",
                            "Connection refused", nullptr};
const char* const kMods[] = {"DuplicateModsFoundException", "ModResolutionException",
                             nullptr};

const Rule kRules[] = {
    {kOom,
     "Minecraft a manqué de mémoire.\n→ Augmente la RAM allouée (Options de "
     "lancement de l'instance, ou Paramètres)."},
    {kJavaVer,
     "La version de Java ne correspond pas à cette version de Minecraft.\n→ "
     "Laisse le launcher télécharger le bon Java, ou installe-le depuis "
     "adoptium.net."},
    {kMissingClass,
     "Un mod est manquant, corrompu ou incompatible.\n→ Retire le dernier mod "
     "ajouté, ou répare l'instance."},
    {kSession,
     "Ta session de jeu a expiré.\n→ Relance simplement : le launcher se "
     "reconnecte automatiquement à Microsoft."},
    {kGl,
     "Problème de pilote graphique.\n→ Mets à jour les pilotes de ta carte "
     "graphique (NVIDIA / AMD / Intel)."},
    {kHeap,
     "Pas assez de RAM libre pour la quantité demandée.\n→ Réduis la RAM allouée "
     "dans les Options de lancement."},
    {kDenied,
     "Un fichier du jeu est bloqué (antivirus ?).\n→ Ajoute une exclusion pour le "
     "dossier Team Launcher dans ton antivirus."},
    {kNet,
     "Problème de connexion Internet pendant le chargement.\n→ Vérifie ta "
     "connexion et relance."},
    {kMods,
     "Conflit entre mods (doublon ou incompatibilité).\n→ Retire les doublons du "
     "dossier mods de l'instance."},
};

bool contains_ci(const std::string& hay, const char* needle) {
    const size_t n = std::char_traits<char>::length(needle);
    if (n == 0 || hay.size() < n) return false;
    const auto eq = [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) ==
               std::tolower(static_cast<unsigned char>(b));
    };
    return std::search(hay.begin(), hay.end(), needle, needle + n, eq) != hay.end();
}

std::string read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

} // namespace

std::optional<std::string> analyze(const std::string& logText) {
    for (const auto& r : kRules)
        for (const char* const* n = r.needles; *n; ++n)
            if (contains_ci(logText, *n)) return std::string(r.advice);
    return std::nullopt;
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

std::optional<std::string> analyze_instance(const fs::path& gameDir) {
    std::error_code ec;

    // 1. rapport de crash officiel (le plus parlant), s'il date de < 5 min
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
                auto found = analyze(read_all(newest));
                return found ? *found
                             : std::string("Minecraft a planté (rapport : "
                                           "crash-reports).");
            }
        }
    }

    // 2. fin du journal du jeu
    const fs::path gameLog = gameDir / "game-log.txt";
    if (fs::exists(gameLog, ec)) return analyze(tail_lines(gameLog, 300));

    return std::nullopt;
}

} // namespace tl::crash

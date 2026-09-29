#include "fonts.hpp"

#include "ui_internal.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>

namespace fs = std::filesystem;

namespace tl::ui::fonts {

namespace {

std::atomic<bool> g_rebuild{false};

// Candidates par plateforme, de la plus lisible a la plus universelle. On
// prend la premiere presente : inutile de demander a l'utilisateur de choisir
// pour que ca marche.
struct Candidate {
    const char* path;
    const char* label;
};

#ifdef _WIN32
const Candidate kCandidates[] = {
    {"C:/Windows/Fonts/segoeui.ttf", "Segoe UI"},
    {"C:/Windows/Fonts/calibri.ttf", "Calibri"},
    {"C:/Windows/Fonts/tahoma.ttf", "Tahoma"},
    {"C:/Windows/Fonts/verdana.ttf", "Verdana"},
    {"C:/Windows/Fonts/arial.ttf", "Arial"},
    // Monospace : pratique pour les journaux et les consoles.
    {"C:/Windows/Fonts/consola.ttf", "Consolas"},
};
#else
const Candidate kCandidates[] = {
    {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "DejaVu Sans"},
    {"/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf", "Noto Sans"},
    {"/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
     "Liberation Sans"},
    {"/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf", "Ubuntu"},
    {"/usr/share/fonts/TTF/DejaVuSans.ttf", "DejaVu Sans"},
    {"/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf", "DejaVu Sans Mono"},
};
#endif

bool readable(const char* p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec) && fs::file_size(p, ec) > 0;
}

// Plage de glyphes : Latin-1 comme avant, PLUS la ponctuation typographique
// reellement employee dans l'interface. Sans elle, « ... » et « — »
// s'affichaient en « ? ». La liste est explicite plutot que « tout
// l'Unicode » : chaque glyphe supplementaire occupe de la place dans
// l'atlas, et le reste ne sert a rien ici.
const ImWchar* ranges() {
    static const ImWchar r[] = {
        0x0020, 0x00FF, // latin de base + supplement (accents francais)
        0x0100, 0x017F, // latin etendu A (pseudos d'autres langues)
        0x2010, 0x2027, // tirets, guillemets simples, points de suspension
        0x2030, 0x205E, // pour mille, primes, guillemets doubles
        0x20A0, 0x20BF, // symboles monetaires (euro)
        0x2190, 0x21FF, // fleches
        0x2200, 0x22FF, // operateurs mathematiques (multiplication, environ)
        0x2500, 0x257F, // filets (arborescences en mode texte)
        0x25A0, 0x25FF, // formes geometriques (puces, triangles)
        0x2600, 0x26FF, // symboles divers
        0,
    };
    return r;
}

std::vector<Choice> g_choices;

} // namespace

const std::vector<Choice>& available() {
    if (!g_choices.empty()) return g_choices;
    g_choices.push_back({"auto", "Automatique"});
    for (const auto& c : kCandidates) {
        if (!readable(c.path)) continue;
        // Une meme police peut apparaitre a deux emplacements selon la
        // distribution : on ne la propose qu'une fois.
        const bool dup = std::any_of(
            g_choices.begin(), g_choices.end(),
            [&](const Choice& x) { return x.label == c.label; });
        if (!dup) g_choices.push_back({c.path, c.label});
    }
    return g_choices;
}

std::string resolve(const std::string& id) {
    if (!id.empty() && id != "auto") {
        if (readable(id.c_str())) return id;
        // Police disparue (desinstallee, ou config venue d'une autre
        // machine) : on ne laisse pas l'interface sans texte, on retombe
        // sur la detection automatique.
    }
    for (const auto& c : kCandidates)
        if (readable(c.path)) return c.path;
    return {};
}

void build(const std::string& id, double scale) {
    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    fBig = fSmall = fTiny = nullptr;

    const float s = static_cast<float>(std::clamp(scale, 0.8, 1.6));
    const std::string path = resolve(id);

    auto add = [&](float px) -> ImFont* {
        const float size = px * s;
        if (!path.empty()) {
            ImFontConfig cfg;
            cfg.OversampleH = 2; // lissage horizontal : texte plus net
            cfg.OversampleV = 1;
            cfg.PixelSnapH = false;
            if (ImFont* f =
                    io.Fonts->AddFontFromFileTTF(path.c_str(), size, &cfg, ranges()))
                return f;
        }
        // Repli ProggyClean : bitmap, mais l'interface reste lisible.
        ImFontConfig cfg;
        cfg.SizePixels = size;
        return io.Fonts->AddFontDefault(&cfg);
    };

    add(16.0f);          // corps, police par defaut d'ImGui
    fBig = add(24.0f);   // titres de page, valeurs de statistiques
    fSmall = add(12.0f); // metadonnees des cartes
    fTiny = add(10.0f);  // compteurs

    io.Fonts->Build();
}

void request_rebuild() { g_rebuild = true; }

bool take_rebuild_request() { return g_rebuild.exchange(false); }

} // namespace tl::ui::fonts

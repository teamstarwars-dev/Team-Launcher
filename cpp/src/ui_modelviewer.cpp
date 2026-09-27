#include "ui_internal.hpp"

#include "model3d.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------------
// Visualiseur de modeles (page 14) — portage de ModelViewerPage.cs et
// ModelViewer3D.cs. Rendu par l'algorithme du peintre dans la liste de dessin
// ImGui : pas une ligne d'OpenGL en plus, donc pas un octet de binaire en plus
// pour un rendu equivalent (le C# peignait des polygones GDI+).
//
// CORRECTIFS vs C# (au-dela de ceux listes dans model3d.hpp) :
//  - Le modele tournait autour de l'ORIGINE du monde alors que les
//    coordonnees d'un modele Minecraft vont de 0 a 16 : il partait donc en
//    orbite hors du cadre. La rotation se fait ici autour du centre de la
//    boite englobante.
//  - Le zoom etait fige a 8 : un modele de 16 unites occupait 128 px quelle
//    que soit la taille du cadre. Ajustement automatique au chargement.
//  - Les faces arriere etaient dessinees puis recouvertes : six faces par
//    boite au lieu de trois. Elles sont maintenant ecartees sur le signe de
//    l'aire projetee, ce qui supprime aussi les scintillements entre faces
//    coplanaires.
// ---------------------------------------------------------------------------

namespace tl::ui {

namespace {

struct ViewState {
    model3d::Model model;
    std::string source;      // ce qui est affiche sous le titre
    std::string error;

    float angleX = 0.30f;    // memes valeurs de depart que le C#
    float angleY = 0.50f;
    float zoom = 0.0f;       // 0 = ajuster au prochain rendu
    bool dragging = false;

    bool showEdges = true;
    bool cull = true;
};
ViewState V;

// Les cinq liens de la colonne d'outils du C#, a l'identique.
struct ToolLink {
    const char* label;
    const char* url;
    const char* fr;
    const char* en;
};
const ToolLink kTools[] = {
    {"Blockbench", "https://blockbench.net/", "Éditeur 3D pour Minecraft",
     "3D editor for Minecraft"},
    {"Mine-imator", "https://www.mineimator.com/", "Animations Minecraft",
     "Minecraft animation"},
    {"Cinema 4D", "https://www.maxon.net/", "Rendu pro (gratuit étudiants)",
     "Pro rendering (free for students)"},
    {"Blender", "https://www.blender.org/", "3D gratuit et open source",
     "Free and open source 3D"},
    {"Pixel Studio", "https://editor.paradulse.net/", "Éditeur pixel art 3D",
     "3D pixel art editor"},
};

std::uint32_t shade(std::uint32_t argb, float factor) {
    const int a = (argb >> 24) & 0xFF;
    auto ch = [&](int shift) {
        const float v = static_cast<float>((argb >> shift) & 0xFF) * factor;
        return static_cast<int>(std::clamp(v, 0.0f, 255.0f));
    };
    // ImGui attend du ABGR (IM_COL32 place le rouge en premier octet).
    return IM_COL32(ch(16), ch(8), ch(0), a);
}

void load_model(const std::filesystem::path& p) {
    V.error.clear();
    auto m = model3d::load(p);
    if (!m) {
        V.error = std::string(tr("Fichier illisible : ", "Cannot read file: ")) +
                  p.filename().string();
        return;
    }
    if (m->empty()) {
        V.error =
            std::string(tr("Aucune boîte exploitable dans ", "No usable box in ")) +
            p.filename().string() +
            tr(" (formats acceptés : .bbmodel, .json, .geo.json).",
               " (accepted formats: .bbmodel, .json, .geo.json).");
        return;
    }
    V.model = std::move(*m);
    V.source = p.filename().string();
    V.zoom = 0.0f; // reajuster
}

void draw_viewport() {
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 size(avail.x, (std::max)(avail.y - 6.0f, 200.0f));
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + size.x, p0.y + size.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(18, 20, 24, 255));
    dl->AddRect(p0, p1, ImGui::ColorConvertFloat4ToU32(kBorder));

    ImGui::InvisibleButton("##modelview", size,
                           ImGuiButtonFlags_MouseButtonLeft |
                               ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();

    if (V.model.empty()) {
        const char* msg = tr("Aucun modèle chargé.", "No model loaded.");
        const ImVec2 ts = ImGui::CalcTextSize(msg);
        dl->AddText(ImVec2((p0.x + p1.x - ts.x) * 0.5f, (p0.y + p1.y - ts.y) * 0.5f),
                    ImGui::ColorConvertFloat4ToU32(kDim), msg);
        return;
    }

    const auto b = model3d::bounds_of(V.model);
    if (V.zoom <= 0.0f)
        V.zoom = (std::min)(size.x, size.y) / (b.size() * 1.6f);

    if (hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f)
            V.zoom = std::clamp(V.zoom * (wheel > 0 ? 1.15f : 1.0f / 1.15f), 0.2f,
                                400.0f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetIO().MouseDelta;
            V.angleY += d.x * 0.01f;
            V.angleX += d.y * 0.01f;
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            const float d = ImGui::GetIO().MouseDelta.y;
            V.zoom = std::clamp(V.zoom * (1.0f + d * 0.01f), 0.2f, 400.0f);
        }
    }

    const float cosX = std::cos(V.angleX), sinX = std::sin(V.angleX);
    const float cosY = std::cos(V.angleY), sinY = std::sin(V.angleY);
    const float ox = (p0.x + p1.x) * 0.5f;
    const float oy = (p0.y + p1.y) * 0.5f;

    struct Face {
        ImVec2 pt[4];
        float depth;
        ImU32 color;
    };
    static std::vector<Face> faces;
    faces.clear();
    faces.reserve(V.model.boxes.size() * 6);

    static const int kIdx[6][4] = {
        {0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7},
        {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0},
    };
    // Memes facteurs d'eclairage que le C# (avant / arriere / gauche / droite
    // / dessus / dessous).
    static const float kShade[6] = {1.0f, 0.70f, 0.85f, 0.80f, 1.10f, 0.60f};

    for (const auto& box : V.model.boxes) {
        const float hw = box.w * 0.5f, hh = box.h * 0.5f, hd = box.d * 0.5f;
        // Centre de la boite, RELATIF au centre du modele : sans ce recentrage
        // le modele orbitait hors du cadre (defaut du C#).
        const float ex = box.x + hw - b.cx();
        const float ey = box.y + hh - b.cy();
        const float ez = box.z + hd - b.cz();
        const float corner[8][3] = {
            {-hw, -hh, -hd}, {hw, -hh, -hd}, {hw, hh, -hd}, {-hw, hh, -hd},
            {-hw, -hh, hd},  {hw, -hh, hd},  {hw, hh, hd},  {-hw, hh, hd},
        };
        for (int f = 0; f < 6; ++f) {
            Face face;
            face.depth = 0.0f;
            for (int c = 0; c < 4; ++c) {
                const float* k = corner[kIdx[f][c]];
                const float wx = ex + k[0], wy = ey + k[1], wz = ez + k[2];
                const float rx = wx * cosY + wz * sinY;
                const float rz = -wx * sinY + wz * cosY;
                const float ry = wy * cosX - rz * sinX;
                const float rz2 = wy * sinX + rz * cosX;
                // Y ecran vers le bas : on inverse pour que +Y monte.
                face.pt[c] = ImVec2(ox + rx * V.zoom, oy - ry * V.zoom);
                face.depth += rz2;
            }
            face.depth *= 0.25f;
            if (V.cull) {
                // Aire signee du quadrilatere projete : negative = face vue de
                // dos, inutile a dessiner.
                float area = 0.0f;
                for (int c = 0; c < 4; ++c) {
                    const ImVec2& a = face.pt[c];
                    const ImVec2& d = face.pt[(c + 1) & 3];
                    area += a.x * d.y - d.x * a.y;
                }
                if (area <= 0.0f) continue;
            }
            face.color = shade(box.color, kShade[f]);
            faces.push_back(face);
        }
    }

    // Algorithme du peintre : du plus loin au plus proche.
    std::sort(faces.begin(), faces.end(),
              [](const Face& a, const Face& c) { return a.depth > c.depth; });

    dl->PushClipRect(p0, p1, true);
    for (const auto& f : faces) {
        dl->AddConvexPolyFilled(f.pt, 4, f.color);
        if (V.showEdges)
            dl->AddPolyline(f.pt, 4, IM_COL32(0, 0, 0, 60), ImDrawFlags_Closed,
                            1.0f);
    }
    dl->PopClipRect();

    char info[160];
    std::snprintf(info, sizeof(info), "%zu %s  ·  %zu %s  ·  zoom %.1f",
                  V.model.boxes.size(), tr("boîte(s)", "box(es)"), faces.size(),
                  tr("face(s)", "face(s)"), V.zoom);
    dl->AddText(ImVec2(p0.x + 8, p0.y + 6), IM_COL32(200, 200, 200, 150), info);
}

} // namespace

void modelviewer_page() {
    static bool first = true;
    if (first) {
        first = false;
        V.model = model3d::mannequin();
        V.source = tr("modèle de démonstration", "demo model");
        if (const char* p = std::getenv("TL_AUTO_MODEL")) load_model(p);
    }

    if (fBig) ImGui::PushFont(fBig);
    ImGui::TextUnformatted(tr("Visualiseur de modèles"));
    if (fBig) ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    ImGui::TextWrapped(
        "%s",
        tr("Ouvre un .bbmodel, un modèle Java (.json) ou une géométrie Bedrock "
           "(.geo.json). Glisse pour tourner, molette pour zoomer. Les "
           "rotations d'éléments et les textures ne sont pas rendues.",
           "Open a .bbmodel, a Java model (.json) or Bedrock geometry "
           "(.geo.json). Drag to rotate, scroll to zoom. Element rotations and "
           "textures are not rendered."));
    ImGui::PopStyleColor();
    ImGui::Spacing();

    if (ImGui::Button(tr("Ouvrir un modèle", "Open a model"), ImVec2(180, 32))) {
        if (auto p = pick_model_file()) load_model(*p);
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Modèle de démonstration", "Demo model"), ImVec2(220, 32))) {
        V.model = model3d::mannequin();
        V.source = tr("modèle de démonstration", "demo model");
        V.error.clear();
        V.zoom = 0.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Réinitialiser la vue", "Reset view"), ImVec2(190, 32))) {
        V.angleX = 0.30f;
        V.angleY = 0.50f;
        V.zoom = 0.0f;
    }
    // Les deux cases sur leur propre ligne : a 960 px la barre debordait.
    ImGui::Checkbox(tr("Arêtes", "Edges"), &V.showEdges);
    ImGui::SameLine();
    ImGui::Checkbox(tr("Faces arrière masquées", "Backface culling"), &V.cull);

    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    if (!V.model.empty())
        ImGui::Text("%s  ·  %s  ·  %s", V.source.c_str(),
                    V.model.format.c_str(),
                    V.model.name.empty() ? "-" : V.model.name.c_str());
    if (V.model.ignoredRotations > 0)
        ImGui::Text(tr("%d élément(s) ont une rotation, ignorée par ce rendu.",
                       "%d element(s) have a rotation, ignored by this view."),
                    V.model.ignoredRotations);
    ImGui::PopStyleColor();
    if (!V.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kDanger);
        ImGui::TextWrapped("%s", V.error.c_str());
        ImGui::PopStyleColor();
    }

    // --- outils externes (les memes cinq liens que le C#) ---
    if (ImGui::CollapsingHeader(tr("Outils externes", "External tools"))) {
        for (const auto& t : kTools) {
            if (ImGui::Button(t.label, ImVec2(150, 26))) open_url(t.url);
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextUnformatted(tr(t.fr, t.en));
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    draw_viewport();
}

} // namespace tl::ui

#include "icons.hpp"

#include "ui_internal.hpp"

#include <algorithm>
#include <cmath>

namespace tl::ui::icons {

namespace {

// Repere de dessin : tout est decrit sur une grille 24x24, `P` ramene un
// point de cette grille aux coordonnees ecran. Ecrire les icones a taille
// fixe puis les mettre a l'echelle evite d'avoir a raisonner en pixels.
struct Grid {
    ImVec2 o;
    float k;
    ImVec2 operator()(float x, float y) const {
        return ImVec2(o.x + x * k, o.y + y * k);
    }
};

void poly(ImDrawList* dl, const Grid& g, const float (*pts)[2], int n, ImU32 col,
          float th, bool closed) {
    ImVec2 buf[16];
    n = (std::min)(n, 16);
    for (int i = 0; i < n; ++i) buf[i] = g(pts[i][0], pts[i][1]);
    dl->AddPolyline(buf, n, col, closed ? ImDrawFlags_Closed : 0, th);
}

void rect(ImDrawList* dl, const Grid& g, float x0, float y0, float x1, float y1,
          ImU32 col, float th, float r = 0.0f) {
    dl->AddRect(g(x0, y0), g(x1, y1), col, r * g.k, 0, th);
}

} // namespace

void draw(ImDrawList* dl, Id id, ImVec2 pos, float size, ImU32 col,
          float thickness) {
    const Grid g{pos, size / 24.0f};
    // Epaisseur proportionnelle : 2/24 de la taille, jamais sous 1 px sinon
    // le trait disparait aux petites tailles.
    const float th = thickness > 0.0f ? thickness
                                      : (std::max)(size * (2.0f / 24.0f), 1.0f);

    switch (id) {
        case Id::Home: {
            const float roof[][2] = {{3, 11}, {12, 3}, {21, 11}};
            poly(dl, g, roof, 3, col, th, false);
            const float body[][2] = {{5, 10}, {5, 21}, {19, 21}, {19, 10}};
            poly(dl, g, body, 4, col, th, false);
            rect(dl, g, 10, 14, 14, 21, col, th);
            break;
        }
        case Id::Instances:
            rect(dl, g, 3, 3, 11, 11, col, th, 1.5f);
            rect(dl, g, 13, 3, 21, 11, col, th, 1.5f);
            rect(dl, g, 3, 13, 11, 21, col, th, 1.5f);
            rect(dl, g, 13, 13, 21, 21, col, th, 1.5f);
            break;
        case Id::Explore: {
            dl->AddCircle(g(12, 12), 9 * g.k, col, 0, th);
            // Aiguille en cerf-volant, moitie nord PLEINE. Les deux versions
            // precedentes (losange au trait, puis eclat trop fin) se
            // lisaient toutes deux comme un panneau de sens interdit : il
            // faut une aiguille large, pas une diagonale.
            ImVec2 north[3] = {g(12, 4.5f), g(15.5f, 12), g(8.5f, 12)};
            dl->AddConvexPolyFilled(north, 3, col);
            ImVec2 south[3] = {g(12, 19.5f), g(8.5f, 12), g(15.5f, 12)};
            dl->AddPolyline(south, 3, col, ImDrawFlags_Closed, th);
            break;
        }
        case Id::Files: {
            const float f[][2] = {{3, 6}, {9, 6}, {11, 9}, {21, 9}, {21, 19},
                                  {3, 19}};
            poly(dl, g, f, 6, col, th, true);
            break;
        }
        case Id::Map: {
            const float m[][2] = {{3, 6}, {9, 3}, {15, 6}, {21, 3}, {21, 18},
                                  {15, 21}, {9, 18}, {3, 21}};
            poly(dl, g, m, 8, col, th, true);
            dl->AddLine(g(9, 3), g(9, 18), col, th);
            dl->AddLine(g(15, 6), g(15, 21), col, th);
            break;
        }
        case Id::City:
            rect(dl, g, 3, 10, 10, 21, col, th);
            rect(dl, g, 12, 5, 21, 21, col, th);
            dl->AddLine(g(14.5f, 8), g(18.5f, 8), col, th);
            dl->AddLine(g(14.5f, 12), g(18.5f, 12), col, th);
            dl->AddLine(g(14.5f, 16), g(18.5f, 16), col, th);
            dl->AddLine(g(5.5f, 14), g(7.5f, 14), col, th);
            break;
        case Id::ModDev: {
            // Marteau : manche en diagonale, tete en haut a droite.
            dl->AddLine(g(4, 20), g(13, 11), col, th);
            const float head[][2] = {{11, 6}, {18, 3}, {21, 8}, {14, 12}};
            poly(dl, g, head, 4, col, th, true);
            break;
        }
        case Id::Model: {
            // Cube isometrique : dessus, puis les deux faces visibles.
            const float top[][2] = {{12, 3}, {21, 8}, {12, 13}, {3, 8}};
            poly(dl, g, top, 4, col, th, true);
            dl->AddLine(g(3, 8), g(3, 16), col, th);
            dl->AddLine(g(21, 8), g(21, 16), col, th);
            dl->AddLine(g(12, 13), g(12, 21), col, th);
            const float b[][2] = {{3, 16}, {12, 21}, {21, 16}};
            poly(dl, g, b, 3, col, th, false);
            break;
        }
        case Id::Play: {
            // Plein : c'est l'action principale, elle doit peser.
            ImVec2 t[3] = {g(7, 4), g(20, 12), g(7, 20)};
            dl->AddConvexPolyFilled(t, 3, col);
            break;
        }
        case Id::Servers:
            rect(dl, g, 3, 4, 21, 10, col, th, 1.5f);
            rect(dl, g, 3, 14, 21, 20, col, th, 1.5f);
            dl->AddCircleFilled(g(6.5f, 7), (std::max)(1.0f * g.k, 1.0f), col);
            dl->AddCircleFilled(g(6.5f, 17), (std::max)(1.0f * g.k, 1.0f), col);
            break;
        case Id::Skins:
        case Id::Account:
            dl->AddCircle(g(12, 8), 4.5f * g.k, col, 0, th);
            {
                const float sh[][2] = {{4, 21}, {4, 18}, {8, 15},
                                       {16, 15}, {20, 18}, {20, 21}};
                poly(dl, g, sh, 6, col, th, false);
            }
            break;
        case Id::News:
            rect(dl, g, 3, 5, 21, 20, col, th, 1.5f);
            dl->AddLine(g(6, 9), g(12, 9), col, th);
            dl->AddLine(g(6, 13), g(18, 13), col, th);
            dl->AddLine(g(6, 16.5f), g(18, 16.5f), col, th);
            rect(dl, g, 14, 8, 18, 11, col, th);
            break;
        case Id::Bedrock: {
            // Bloc PLEIN, silhouette hexagonale. Modeles 3D utilise le meme
            // volume en fil de fer : remplir entierement est la seule
            // difference qui reste lisible a 16 px (une simple face du
            // dessus remplie ne se voyait pas).
            ImVec2 hex[6] = {g(12, 3), g(21, 8),  g(21, 16),
                             g(12, 21), g(3, 16), g(3, 8)};
            dl->AddConvexPolyFilled(hex, 6, col);
            break;
        }
        case Id::Settings: {
            dl->AddCircle(g(12, 12), 3.5f * g.k, col, 0, th);
            dl->AddCircle(g(12, 12), 8.5f * g.k, col, 0, th);
            // Quatre dents : suffisant pour lire « engrenage » a 16 px.
            for (int i = 0; i < 4; ++i) {
                const float a = static_cast<float>(i) * 1.57079633f + 0.7853982f;
                const ImVec2 c = g(12, 12);
                const float r0 = 8.0f * g.k, r1 = 11.0f * g.k;
                dl->AddLine(ImVec2(c.x + std::cos(a) * r0, c.y + std::sin(a) * r0),
                            ImVec2(c.x + std::cos(a) * r1, c.y + std::sin(a) * r1),
                            col, th * 1.6f);
            }
            break;
        }
        case Id::Download: {
            dl->AddLine(g(12, 3), g(12, 15), col, th);
            const float a[][2] = {{7, 10}, {12, 15}, {17, 10}};
            poly(dl, g, a, 3, col, th, false);
            const float tray[][2] = {{4, 17}, {4, 21}, {20, 21}, {20, 17}};
            poly(dl, g, tray, 4, col, th, false);
            break;
        }
        case Id::Help: {
            dl->AddCircle(g(12, 12), 9 * g.k, col, 0, th);
            const float q[][2] = {{9, 9}, {12, 6.5f}, {15, 9}, {12, 12}, {12, 14.5f}};
            poly(dl, g, q, 5, col, th, false);
            dl->AddCircleFilled(g(12, 18), (std::max)(1.1f * g.k, 1.0f), col);
            break;
        }
        case Id::Search:
            dl->AddCircle(g(10.5f, 10.5f), 6.5f * g.k, col, 0, th);
            dl->AddLine(g(15.5f, 15.5f), g(21, 21), col, th * 1.2f);
            break;
        case Id::Check: {
            const float c[][2] = {{4, 13}, {9.5f, 18.5f}, {20, 6}};
            poly(dl, g, c, 3, col, th * 1.3f, false);
            break;
        }
        case Id::Close:
            dl->AddLine(g(6, 6), g(18, 18), col, th * 1.2f);
            dl->AddLine(g(18, 6), g(6, 18), col, th * 1.2f);
            break;
        case Id::Pause:
            dl->AddRectFilled(g(7.5f, 5), g(10.5f, 19), col);
            dl->AddRectFilled(g(13.5f, 5), g(16.5f, 19), col);
            break;
        case Id::Trash: {
            dl->AddLine(g(4, 7), g(20, 7), col, th);
            const float lid[][2] = {{9, 7}, {9, 4}, {15, 4}, {15, 7}};
            poly(dl, g, lid, 4, col, th, false);
            const float body[][2] = {{6, 7}, {7, 21}, {17, 21}, {18, 7}};
            poly(dl, g, body, 4, col, th, false);
            dl->AddLine(g(10, 11), g(10.5f, 17.5f), col, th);
            dl->AddLine(g(14, 11), g(13.5f, 17.5f), col, th);
            break;
        }
        case Id::Star: {
            // Etoile a cinq branches, rayons 9 et 3,8.
            ImVec2 p[10];
            for (int i = 0; i < 10; ++i) {
                const float a = -1.57079633f + static_cast<float>(i) * 0.62831853f;
                const float r = (i % 2 == 0 ? 9.0f : 3.8f);
                p[i] = g(12 + std::cos(a) * r, 12 + std::sin(a) * r);
            }
            dl->AddPolyline(p, 10, col, ImDrawFlags_Closed, th);
            break;
        }
        case Id::Folder: {
            const float f[][2] = {{3, 7}, {9, 7}, {11, 10}, {21, 10}};
            poly(dl, g, f, 4, col, th, false);
            const float b[][2] = {{3, 7}, {3, 20}, {21, 20}, {21, 10}};
            poly(dl, g, b, 4, col, th, false);
            break;
        }
    }
}

bool nav_item(Id id, const char* label, bool active, bool compact) {
    const ImGuiStyle& st = ImGui::GetStyle();
    const float h = ImGui::GetFontSize() + st.FramePadding.y * 2.0f;
    const float iconSize = ImGui::GetFontSize() * 1.15f;
    const float w = compact ? h : ImGui::GetContentRegionAvail().x;

    ImGui::PushStyleColor(ImGuiCol_Button, active ? kAccent : kCard);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          active ? kAccent : ImVec4(kCard.x + 0.05f, kCard.y + 0.05f,
                                                    kCard.z + 0.05f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccent);
    // Libelle aligne a gauche : en mode large l'icone occupe la marge gauche.
    ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    std::string id_str = std::string("##nav") + label;
    // En mode large on reserve la place de l'icone par des espaces : ImGui
    // n'a pas de bouton « icone + texte », et decaler le texte avec
    // FramePadding decalerait aussi le fond du bouton.
    std::string shown = id_str;
    if (!compact) shown = std::string("      ") + label + id_str;

    const bool clicked = ImGui::Button(shown.c_str(), ImVec2(w, h));
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

    const ImU32 col = ImGui::ColorConvertFloat4ToU32(
        active ? ImVec4(1, 1, 1, 1) : kText);
    const float pad = compact ? (h - iconSize) * 0.5f : st.FramePadding.x;
    draw(ImGui::GetWindowDrawList(), id,
         ImVec2(p0.x + pad, p0.y + (h - iconSize) * 0.5f), iconSize, col);

    // Infobulle : indispensable en mode compact, ou rien n'est ecrit.
    if (compact && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", label);
    return clicked;
}

} // namespace tl::ui::icons

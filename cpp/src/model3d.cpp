#include "model3d.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

namespace tl::model3d {

namespace {

std::uint32_t rgb(int r, int g, int b, int a = 255) {
    return (static_cast<std::uint32_t>(a) << 24) |
           (static_cast<std::uint32_t>(r) << 16) |
           (static_cast<std::uint32_t>(g) << 8) | static_cast<std::uint32_t>(b);
}

// Teinte de repli, comme le C# : une roue de 16 nuances indexee sur le rang.
std::uint32_t fallback_color(std::size_t index) {
    const int i = static_cast<int>(index % 16);
    return rgb(100 + i * 8, 80 + i * 12, 60 + i * 10, 200);
}

bool num3(const json& j, float out[3]) {
    if (!j.is_array() || j.size() < 3) return false;
    for (int i = 0; i < 3; ++i) {
        if (!j[i].is_number()) return false;
        out[i] = j[i].get<float>();
    }
    return true;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Une boite n'est retenue que si ses trois dimensions sont strictement
// positives — meme regle que le C#, qui evite les faces degenerees.
void push_box(Model& m, float x, float y, float z, float w, float h, float d,
              std::uint32_t color, bool rotated) {
    if (!(w > 0.0f && h > 0.0f && d > 0.0f)) return;
    Box b;
    b.x = x; b.y = y; b.z = z;
    b.w = w; b.h = h; b.d = d;
    b.color = color;
    b.rotated = rotated;
    m.boxes.push_back(b);
    if (rotated) ++m.ignoredRotations;
}

// `elements` de Blockbench ou d'un modele Java : from/to, sinon origin+size.
void parse_elements(const json& arr, Model& m) {
    if (!arr.is_array()) return;
    for (const auto& el : arr) {
        if (!el.is_object()) continue;
        float from[3], to[3], org[3], size[3];
        const bool rotated = el.contains("rotation");
        std::uint32_t color = fallback_color(m.boxes.size());
        if (const auto it = el.find("color");
            it != el.end() && it->is_number_integer()) {
            const int c = it->get<int>();
            color = rgb((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
        } else if (const auto f = el.find("faces");
                   f != el.end() && f->is_object()) {
            // Le C# teintait en vert une face portant `tintindex` (feuillage).
            for (const auto& [k, v] : f->items()) {
                (void)k;
                if (v.is_object() && v.contains("tintindex")) {
                    color = rgb(140, 180, 100, 200);
                    break;
                }
            }
        }

        const auto itFrom = el.find("from");
        const auto itTo = el.find("to");
        if (itFrom != el.end() && itTo != el.end() && num3(*itFrom, from) &&
            num3(*itTo, to)) {
            push_box(m, (std::min)(from[0], to[0]), (std::min)(from[1], to[1]),
                     (std::min)(from[2], to[2]), std::abs(to[0] - from[0]),
                     std::abs(to[1] - from[1]), std::abs(to[2] - from[2]), color,
                     rotated);
            continue;
        }
        // Ancien format : `origin` est ici le coin, accompagne de `size`.
        const auto itOrg = el.find("origin");
        const auto itSize = el.find("size");
        if (itOrg != el.end() && itSize != el.end() && num3(*itOrg, org) &&
            num3(*itSize, size))
            push_box(m, org[0], org[1], org[2], size[0], size[1], size[2], color,
                     rotated);
    }
}

// `cubes` : origin + size (Blockbench ancien, et os Bedrock).
void parse_cubes(const json& arr, Model& m) {
    if (!arr.is_array()) return;
    for (const auto& c : arr) {
        if (!c.is_object()) continue;
        float org[3], size[3];
        const auto itOrg = c.find("origin");
        const auto itSize = c.find("size");
        if (itOrg == c.end() || itSize == c.end()) continue;
        if (!num3(*itOrg, org) || !num3(*itSize, size)) continue;
        push_box(m, org[0], org[1], org[2], size[0], size[1], size[2],
                 fallback_color(m.boxes.size()), c.contains("rotation"));
    }
}

// Geometrie Bedrock : minecraft:geometry[].bones[].cubes[]
bool parse_bedrock(const json& root, Model& m) {
    const auto g = root.find("minecraft:geometry");
    if (g == root.end() || !g->is_array()) return false;
    for (const auto& geo : *g) {
        if (!geo.is_object()) continue;
        if (m.name.empty()) {
            const auto d = geo.find("description");
            if (d != geo.end() && d->is_object())
                m.name = d->value("identifier", "");
        }
        const auto bones = geo.find("bones");
        if (bones == geo.end() || !bones->is_array()) continue;
        for (const auto& bone : *bones) {
            if (!bone.is_object()) continue;
            const auto c = bone.find("cubes");
            if (c != bone.end()) parse_cubes(*c, m);
        }
    }
    return !m.boxes.empty();
}

} // namespace

float Bounds3::size() const {
    const float s = (std::max)({maxX - minX, maxY - minY, maxZ - minZ});
    return s > 0.0f ? s : 1.0f;
}

Bounds3 bounds_of(const Model& m) {
    Bounds3 b;
    if (m.boxes.empty()) return b;
    b.minX = b.maxX = m.boxes[0].x;
    b.minY = b.maxY = m.boxes[0].y;
    b.minZ = b.maxZ = m.boxes[0].z;
    for (const auto& x : m.boxes) {
        b.minX = (std::min)(b.minX, x.x);
        b.minY = (std::min)(b.minY, x.y);
        b.minZ = (std::min)(b.minZ, x.z);
        b.maxX = (std::max)(b.maxX, x.x + x.w);
        b.maxY = (std::max)(b.maxY, x.y + x.h);
        b.maxZ = (std::max)(b.maxZ, x.z + x.d);
    }
    return b;
}

Model parse(const std::string& text, const std::string& ext) {
    Model m;
    const json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return m;

    const std::string e = lower(ext);
    const bool wantBedrock = e.find("geo") != std::string::npos ||
                             root.contains("minecraft:geometry");

    if (wantBedrock && parse_bedrock(root, m)) {
        m.format = "bedrock";
        return m;
    }

    if (const auto it = root.find("elements"); it != root.end())
        parse_elements(*it, m);
    // Blockbench ancien : les boites sont dans `cubes`. Le C# cherchait aussi
    // une cle « cubes » avec une espace en tete : contournement abandonne.
    if (const auto it = root.find("cubes"); it != root.end())
        parse_cubes(*it, m);

    if (m.boxes.empty()) {
        // Dernier essai : une geometrie Bedrock sans l'extension attendue.
        if (parse_bedrock(root, m)) {
            m.format = "bedrock";
            return m;
        }
        return m;
    }

    m.format = (e.find("bbmodel") != std::string::npos ||
                root.contains("meta") || root.contains("resolution"))
                   ? "bbmodel"
                   : "java";
    if (m.name.empty()) m.name = root.value("name", "");
    return m;
}

std::optional<Model> load(const fs::path& p) {
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return std::nullopt;
    // Un modele legitime pese quelques dizaines de Ko ; au-dela on refuse
    // plutot que de charger un fichier arbitraire en memoire.
    if (fs::file_size(p, ec) > 32u * 1024 * 1024) return std::nullopt;
    std::ifstream in(p, std::ios::binary);
    if (!in) return std::nullopt;
    const std::string text{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    Model m = parse(text, p.filename().string());
    if (m.name.empty()) m.name = p.filename().string();
    return m;
}

Model mannequin() {
    Model m;
    m.format = "defaut";
    m.name = "Personnage";
    const std::uint32_t skin = rgb(200, 160, 120);
    const std::uint32_t shirt = rgb(50, 100, 200);
    const std::uint32_t pants = rgb(50, 50, 150);
    const std::uint32_t shoes = rgb(60, 60, 60);

    // Le C# stockait un CENTRE et une taille ; ici on stocke le coin minimal,
    // d'ou la conversion. Les « yeux » du C# avaient une profondeur nulle :
    // ils etaient donc invisibles (une boite plate n'a aucune face fermee).
    // Ils recoivent une epaisseur pour exister reellement.
    auto centered = [&](float cx, float cy, float cz, float w, float h, float d,
                        std::uint32_t c) {
        push_box(m, cx - w / 2, cy - h / 2, cz - d / 2, w, h, d, c, false);
    };
    centered(0, 6, 0, 4, 4, 4, skin);              // tete
    centered(-1, 7, -2.1f, 1, 1, 0.2f, rgb(255, 255, 255)); // oeil gauche
    centered(1, 7, -2.1f, 1, 1, 0.2f, rgb(255, 255, 255));  // oeil droit
    centered(-1, 7, -2.25f, 0.5f, 0.5f, 0.2f, rgb(0, 0, 0));
    centered(1, 7, -2.25f, 0.5f, 0.5f, 0.2f, rgb(0, 0, 0));
    centered(0, 2, 0, 4, 4, 2, shirt);             // buste
    centered(-3, 2, 0, 2, 4, 2, skin);             // bras
    centered(3, 2, 0, 2, 4, 2, skin);
    centered(-1, -2, 0, 2, 4, 2, pants);           // jambes
    centered(1, -2, 0, 2, 4, 2, pants);
    centered(-1, -4, 0, 2, 1, 3, shoes);           // chaussures
    centered(1, -4, 0, 2, 1, 3, shoes);
    return m;
}

} // namespace tl::model3d

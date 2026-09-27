#include "citygen.hpp"

#include "http_win.hpp"
#include "region.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <system_error>

using nlohmann::json;

namespace tl::citygen {

namespace {

// Overpass rend toujours les valeurs de tags en chaine.
std::string tag(const json& tags, const char* key) {
    if (!tags.is_object()) return {};
    auto it = tags.find(key);
    if (it == tags.end() || !it->is_string()) return {};
    return it->get<std::string>();
}

bool has_tag(const json& tags, const char* key) {
    return tags.is_object() && tags.find(key) != tags.end();
}

} // namespace

BBox parse_bbox(const std::string& s) {
    BBox b;
    std::vector<std::string> parts;
    std::string cur;
    // Le separateur est la virgule ; une virgule decimale casserait tout, on
    // exige donc le point decimal dans une bbox a quatre champs.
    for (char c : s) {
        if (c == ',') {
            parts.push_back(cur);
            cur.clear();
        } else if (c != ' ' && c != '\t') {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    if (parts.size() != 4) return b;
    for (const auto& p : parts)
        if (p.empty()) return b;

    b.minLon = std::atof(parts[0].c_str());
    b.minLat = std::atof(parts[1].c_str());
    b.maxLon = std::atof(parts[2].c_str());
    b.maxLat = std::atof(parts[3].c_str());
    // Bornes geographiques + ordre coherent.
    if (b.minLon < -180 || b.maxLon > 180 || b.minLat < -90 || b.maxLat > 90)
        return b;
    if (b.minLon >= b.maxLon || b.minLat >= b.maxLat) return b;
    b.valid = true;
    return b;
}

const char* block_for(Kind k) {
    switch (k) {
    case Kind::Building: return "minecraft:stone";
    case Kind::Highway: return "minecraft:gravel";
    case Kind::Water: return "minecraft:water";
    case Kind::Park: return "minecraft:grass_block";
    case Kind::Railway: return "minecraft:iron_block";
    default: return "minecraft:stone";
    }
}

OsmData parse_overpass(const std::string& body, const BBox& box) {
    OsmData data;
    if (!box.valid) return data;
    data.centerLon = (box.minLon + box.maxLon) / 2.0;
    data.centerLat = (box.minLat + box.maxLat) / 2.0;

    json doc;
    try {
        doc = json::parse(body);
    } catch (const json::exception&) {
        return data;
    }
    auto els = doc.find("elements");
    if (els == doc.end() || !els->is_array()) return data;

    // Projection equirectangulaire locale : 1 degre de latitude ~ 111 320 m,
    // la longitude etant resserree par le cosinus de la latitude centrale.
    const double lonScale = 111320.0 * std::cos(data.centerLat * 3.14159265358979 / 180.0);
    const double latScale = 111320.0;
    auto lon_to_x = [&](double lon) {
        return static_cast<int>((lon - data.centerLon) * lonScale);
    };
    auto lat_to_z = [&](double lat) {
        return static_cast<int>((lat - data.centerLat) * latScale);
    };

    // 1re passe : les noeuds.
    std::map<std::int64_t, Point> nodes;
    for (const auto& el : *els) {
        if (!el.is_object()) continue;
        if (el.value("type", std::string{}) != "node") continue;
        auto lo = el.find("lon");
        auto la = el.find("lat");
        if (lo == el.end() || la == el.end() || !lo->is_number() || !la->is_number())
            continue;
        nodes[el.value("id", 0LL)] =
            Point{lon_to_x(lo->get<double>()), lat_to_z(la->get<double>())};
    }
    data.nodes = static_cast<int>(nodes.size());

    // 2e passe : les chemins.
    for (const auto& el : *els) {
        if (!el.is_object()) continue;
        if (el.value("type", std::string{}) != "way") continue;

        Entity e;
        e.id = el.value("id", 0LL);
        auto nArr = el.find("nodes");
        if (nArr != el.end() && nArr->is_array())
            for (const auto& n : *nArr) {
                if (!n.is_number_integer()) continue;
                auto it = nodes.find(n.get<std::int64_t>());
                if (it != nodes.end()) e.points.push_back(it->second);
            }
        if (e.points.size() < 2) continue;

        const json tags = el.contains("tags") ? el["tags"] : json::object();
        if (has_tag(tags, "building")) {
            e.kind = Kind::Building;
            e.height = 4; // defaut du C#
            const std::string lv = tag(tags, "building:levels");
            if (!lv.empty()) {
                const int levels = std::atoi(lv.c_str());
                if (levels > 0) e.height = (std::min)(levels, 64) * 4;
            }
        } else if (has_tag(tags, "highway")) {
            e.kind = Kind::Highway;
            const std::string hw = tag(tags, "highway");
            e.width = (hw == "primary" || hw == "secondary" || hw == "tertiary") ? 6 : 3;
        } else if (has_tag(tags, "waterway") ||
                   tag(tags, "natural") == "water") {
            // CORRECTIF vs C# : il testait la seule PRESENCE du tag
            // « natural », donc une foret, une falaise ou une plage
            // devenaient de l'eau. On exige natural=water.
            e.kind = Kind::Water;
        } else if (tag(tags, "leisure") == "park" || has_tag(tags, "landuse")) {
            e.kind = Kind::Park;
        } else if (has_tag(tags, "railway")) {
            e.kind = Kind::Railway;
            e.width = 2;
        } else {
            continue; // chemin sans tag exploitable
        }
        data.entities.push_back(std::move(e));
    }
    return data;
}

OsmData fetch_osm(const BBox& box, const Progress& progress,
                  const std::atomic<bool>* cancel) {
    if (!box.valid)
        throw std::runtime_error(
            "Emprise invalide. Format attendu : minLon,minLat,maxLon,maxLat");
    if (progress) progress("Récupération des données OpenStreetMap...");

    auto num = [](double v) {
        char b[32];
        std::snprintf(b, sizeof(b), "%.6f", v);
        return std::string(b);
    };
    const std::string s = num(box.minLat), w = num(box.minLon);
    const std::string n = num(box.maxLat), e = num(box.maxLon);
    const std::string bb = s + "," + w + "," + n + "," + e;

    const std::string query =
        "[out:json][timeout:120];\n(\n"
        "  way[\"building\"](" + bb + ");\n"
        "  way[\"highway\"](" + bb + ");\n"
        "  way[\"waterway\"](" + bb + ");\n"
        "  way[\"natural\"=\"water\"](" + bb + ");\n"
        "  way[\"leisure\"=\"park\"](" + bb + ");\n"
        "  way[\"landuse\"](" + bb + ");\n"
        "  way[\"railway\"](" + bb + ");\n"
        ");\nout body;\n>;\nout skel qt;";

    auto r = http::post_string("https://overpass-api.de/api/interpreter", query,
                               "text/plain", {}, cancel);
    if (!r) throw std::runtime_error("Échec réseau vers overpass-api.de.");
    if (r->status == 429 || r->status == 504)
        throw std::runtime_error(
            "Overpass est saturé (HTTP " + std::to_string(r->status) +
            ").\nRéessaie dans quelques minutes ou réduis l'emprise.");
    if (r->status != 200)
        throw std::runtime_error("HTTP " + std::to_string(r->status) +
                                 " depuis overpass-api.de.");

    if (progress) progress("Analyse des données...");
    OsmData d = parse_overpass(r->body, box);
    if (d.entities.empty())
        throw std::runtime_error(
            "Aucune donnée exploitable dans cette zone.\nVérifie l'emprise.");
    return d;
}

// --- rasterisation ---------------------------------------------------------

bool point_in_polygon(int px, int pz, const std::vector<Point>& poly) {
    bool in = false;
    const std::size_t n = poly.size();
    for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
        const auto& a = poly[i];
        const auto& b = poly[j];
        if ((a.z > pz) == (b.z > pz)) continue;
        if (a.z == b.z) continue; // segment horizontal : pas de croisement
        const double t = static_cast<double>(pz - a.z) / (b.z - a.z);
        if (px < a.x + t * (b.x - a.x)) in = !in;
    }
    return in;
}

void fill_polygon(std::vector<Block>& out, const std::vector<Point>& poly,
                  int baseY, int height, const char* name) {
    if (poly.size() < 3) return;
    int x0 = poly[0].x, x1 = poly[0].x, z0 = poly[0].z, z1 = poly[0].z;
    for (const auto& p : poly) {
        x0 = (std::min)(x0, p.x);
        x1 = (std::max)(x1, p.x);
        z0 = (std::min)(z0, p.z);
        z1 = (std::max)(z1, p.z);
    }
    // Emprise delirante (donnees aberrantes) : on laisse tomber l'entite
    // plutot que de boucler des milliards de fois.
    const long long area = static_cast<long long>(x1 - x0 + 1) * (z1 - z0 + 1);
    if (area <= 0 || area > 4000000LL) return;

    const int h = (std::max)(height, 1);
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            if (!point_in_polygon(x, z, poly)) continue;
            for (int dy = 0; dy < h; ++dy) out.push_back({x, baseY + dy, z, name});
        }
}

void fill_line(std::vector<Block>& out, const std::vector<Point>& pts, int width,
               int y, const char* name) {
    if (pts.size() < 2) return;
    const int half = (std::max)(width, 1) / 2;
    for (std::size_t i = 0; i + 1 < pts.size(); ++i) {
        const auto a = pts[i];
        const auto b = pts[i + 1];
        const int dx = std::abs(b.x - a.x);
        const int dz = std::abs(b.z - a.z);
        const int steps = (std::max)(dx, dz);
        if (steps > 100000) continue; // segment aberrant
        for (int s = 0; s <= steps; ++s) {
            const double t = steps == 0 ? 0.0 : static_cast<double>(s) / steps;
            const int x = a.x + static_cast<int>((b.x - a.x) * t);
            const int z = a.z + static_cast<int>((b.z - a.z) * t);
            for (int ox = -half; ox <= half; ++ox)
                for (int oz = -half; oz <= half; ++oz)
                    out.push_back({x + ox, y, z + oz, name});
        }
    }
}

std::vector<Block> rasterize(const OsmData& data, int baseY,
                             std::size_t maxBlocks) {
    std::vector<Block> out;
    for (const auto& e : data.entities) {
        if (out.size() >= maxBlocks) break;
        const char* name = block_for(e.kind);
        switch (e.kind) {
        case Kind::Building:
            fill_polygon(out, e.points, baseY, e.height, name);
            break;
        case Kind::Water:
        case Kind::Park:
            fill_polygon(out, e.points, baseY, 1, name);
            break;
        case Kind::Highway:
        case Kind::Railway:
            fill_line(out, e.points, e.width, baseY, name);
            break;
        default: break;
        }
    }
    if (out.size() > maxBlocks) out.resize(maxBlocks);
    return out;
}

PasteResult paste_into_world(const std::filesystem::path& worldDir,
                             const std::vector<Block>& blocks, int originX,
                             int originZ, const Progress& progress,
                             const std::atomic<bool>* cancel) {
    PasteResult r;
    auto say = [&](const std::string& m) { if (progress) progress(m); };
    if (blocks.empty()) return r;

    std::error_code ec;
    if (!std::filesystem::is_directory(worldDir / "region", ec)) {
        r.error = "Le dossier region/ du monde est introuvable.";
        return r;
    }

    // Regroupement par chunk sans dupliquer les blocs : on trie des index.
    struct Ref { std::int64_t key; std::uint32_t idx; };
    std::vector<Ref> refs;
    refs.reserve(blocks.size());
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        const int cx = static_cast<int>(
            std::floor((blocks[i].x + originX) / 16.0));
        const int cz = static_cast<int>(
            std::floor((blocks[i].z + originZ) / 16.0));
        refs.push_back({(static_cast<std::int64_t>(cx) << 32) |
                            static_cast<std::int64_t>(
                                static_cast<std::uint32_t>(cz)),
                        static_cast<std::uint32_t>(i)});
    }
    std::sort(refs.begin(), refs.end(),
              [](const Ref& a, const Ref& b) { return a.key < b.key; });

    std::vector<region::BlockEdit> edits;
    std::size_t done = 0;
    for (std::size_t i = 0; i < refs.size();) {
        if (cancel && cancel->load()) {
            r.error = "Operation annulee.";
            return r;
        }
        std::size_t j = i;
        while (j < refs.size() && refs[j].key == refs[i].key) ++j;

        const int cx = static_cast<int>(refs[i].key >> 32);
        const int cz = static_cast<int>(
            static_cast<std::uint32_t>(refs[i].key & 0xFFFFFFFF));

        auto chunk = region::read_chunk(worldDir, cx, cz);
        if (!chunk) {
            ++r.chunksMissing;
            r.skipped += static_cast<int>(j - i);
            i = j;
            continue;
        }

        edits.clear();
        edits.reserve(j - i);
        for (std::size_t k = i; k < j; ++k) {
            const Block& b = blocks[refs[k].idx];
            const int wx = b.x + originX;
            const int wz = b.z + originZ;
            edits.push_back({((wx % 16) + 16) % 16, b.y, ((wz % 16) + 16) % 16,
                             b.name ? b.name : ""});
        }

        const auto er = region::set_blocks(*chunk, edits);
        r.placed += er.applied;
        r.skipped += er.skipped;
        r.unsupported += er.unsupported;
        if (er.applied > 0) {
            if (region::write_chunk(worldDir, cx, cz, *chunk)) {
                ++r.chunksWritten;
            } else {
                r.error = "Echec d'ecriture du chunk (" + std::to_string(cx) +
                          ", " + std::to_string(cz) + ").";
                return r;
            }
        }

        done += j - i;
        if (r.chunksWritten > 0 && r.chunksWritten % 32 == 0) {
            say("Ecriture : " + std::to_string(done) + "/" +
                std::to_string(blocks.size()) + " blocs, " +
                std::to_string(r.chunksWritten) + " chunk(s)");
        }
        i = j;
    }
    say("Termine : " + std::to_string(r.placed) + " bloc(s) pose(s) dans " +
        std::to_string(r.chunksWritten) + " chunk(s)");
    return r;
}

} // namespace tl::citygen

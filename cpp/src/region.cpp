#include "region.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <set>

namespace fs = std::filesystem;

namespace tl::region {

namespace {

constexpr std::size_t kSector = 4096;
constexpr std::size_t kHeader = 2 * kSector; // offsets + horodatages

std::uint32_t be24(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) |
           (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}
std::uint32_t be32(const std::uint8_t* p) {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}

std::vector<std::uint8_t> read_all(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const std::streamoff n = in.tellg();
    if (n <= 0) return {};
    in.seekg(0, std::ios::beg);
    std::vector<std::uint8_t> v(static_cast<std::size_t>(n));
    in.read(reinterpret_cast<char*>(v.data()), n);
    return in ? v : std::vector<std::uint8_t>{};
}

// Sections du chunk, quel que soit l'emplacement selon la version :
// 1.18+ à la racine sous « sections », avant sous « Level »/« Sections ».
const nbt::List* sections_of(const nbt::Compound& chunk) {
    if (const nbt::List* l = nbt::get_list(chunk, "sections")) return l;
    if (const nbt::Compound* lvl = nbt::get_compound(chunk, "Level"))
        if (const nbt::List* l = nbt::get_list(*lvl, "Sections")) return l;
    return nullptr;
}

std::int64_t data_version(const nbt::Compound& chunk) {
    const std::int64_t v = nbt::get_num(chunk, "DataVersion", -1);
    if (v >= 0) return v;
    if (const nbt::Compound* lvl = nbt::get_compound(chunk, "Level"))
        return nbt::get_num(*lvl, "DataVersion", -1);
    return -1;
}

// Nom d'un element de palette ({Name: "minecraft:stone", Properties: {...}}).
std::string palette_name(const nbt::Tag& t) {
    if (t.type != nbt::Type::Compound || !t.comp) return "minecraft:air";
    return nbt::get_string(*t.comp, "Name", "minecraft:air");
}

// Depaquetage d'un indice de palette dans un tableau de long.
// packed = true  : entrees tassees, pouvant chevaucher deux long (1.13–1.15)
// packed = false : chaque long rembourre, aucune entree a cheval (1.16+)
int palette_index(const std::vector<std::int64_t>& data, int i, int bits,
                  bool packed) {
    if (bits <= 0 || bits > 32) return -1;
    const std::uint64_t mask = (bits == 64) ? ~0ull : ((1ull << bits) - 1);
    if (packed) {
        const std::size_t bitPos = static_cast<std::size_t>(i) * bits;
        const std::size_t idx = bitPos >> 6;
        const int off = static_cast<int>(bitPos & 63);
        if (idx >= data.size()) return -1;
        std::uint64_t v = static_cast<std::uint64_t>(data[idx]) >> off;
        if (off + bits > 64) { // l'entree deborde sur le long suivant
            if (idx + 1 >= data.size()) return -1;
            v |= static_cast<std::uint64_t>(data[idx + 1]) << (64 - off);
        }
        return static_cast<int>(v & mask);
    }
    const int per = 64 / bits;
    const std::size_t idx = static_cast<std::size_t>(i / per);
    const int off = (i % per) * bits;
    if (idx >= data.size()) return -1;
    return static_cast<int>((static_cast<std::uint64_t>(data[idx]) >> off) & mask);
}

int bits_for(std::size_t paletteSize, int minBits) {
    int bits = minBits;
    while ((1u << bits) < paletteSize) ++bits;
    return bits;
}

// Une section decodee : noms de palette + indices, ou bloc unique.
struct Section {
    int y = 0;
    std::vector<std::string> palette;
    std::vector<std::int64_t> data;
    int bits = 0;
    bool packed = false;
    bool single = false; // palette a une entree : toute la section identique
    bool valid = false;
};

Section decode_section(const nbt::Compound& sec, std::int64_t dataVersion) {
    Section s;
    // « Y » est un TAG_Byte SIGNE : le C# le lisait non signe, une section a
    // -1 devenait 255 (y = 4080).
    s.y = static_cast<int>(nbt::get_num(sec, "Y", 0));

    // --- 1.18+ : block_states { palette, data } ---
    if (const nbt::Compound* bs = nbt::get_compound(sec, "block_states")) {
        const nbt::List* pal = nbt::get_list(*bs, "palette");
        if (!pal || pal->empty()) return s;
        for (const auto& t : *pal) s.palette.push_back(palette_name(t));
        if (const nbt::Tag* d = nbt::find(*bs, "data");
            d && d->type == nbt::Type::LongArray)
            s.data = d->longs;
        s.single = s.palette.size() == 1 || s.data.empty();
        s.bits = bits_for(s.palette.size(), 4);
        s.packed = false; // 1.18 est posterieur a 1.16
        s.valid = true;
        return s;
    }

    // --- 1.13–1.17 : Palette + BlockStates (absent du portage C#) ---
    if (const nbt::List* pal = nbt::get_list(sec, "Palette")) {
        if (pal->empty()) return s;
        for (const auto& t : *pal) s.palette.push_back(palette_name(t));
        if (const nbt::Tag* d = nbt::find(sec, "BlockStates");
            d && d->type == nbt::Type::LongArray)
            s.data = d->longs;
        s.single = s.palette.size() == 1 || s.data.empty();
        s.bits = bits_for(s.palette.size(), 4);
        // 2529 = 1.16 : a partir de la, les entrees ne chevauchent plus.
        s.packed = dataVersion >= 0 && dataVersion < 2529;
        s.valid = true;
        return s;
    }

    // --- <= 1.12 : Blocks (+ Add) + Data, identifiants numeriques ---
    if (const nbt::Tag* blk = nbt::find(sec, "Blocks");
        blk && blk->type == nbt::Type::ByteArray && blk->bytes.size() >= 4096) {
        s.valid = true;
        s.bits = -1; // marqueur « ancien format »
        return s;
    }
    return s;
}

// <= 1.12 : identifiant numerique du bloc en position i (0-4095).
int legacy_block_id(const nbt::Compound& sec, int i) {
    const nbt::Tag* blk = nbt::find(sec, "Blocks");
    if (!blk || blk->bytes.size() <= static_cast<std::size_t>(i)) return -1;
    int id = blk->bytes[static_cast<std::size_t>(i)];
    // « Add » : demi-octet d'extension au-dela de 255 (rare mais legal).
    if (const nbt::Tag* add = nbt::find(sec, "Add");
        add && add->type == nbt::Type::ByteArray &&
        add->bytes.size() > static_cast<std::size_t>(i / 2)) {
        const int nib = (i & 1) == 0 ? (add->bytes[i / 2] & 0x0F)
                                     : ((add->bytes[i / 2] >> 4) & 0x0F);
        id |= nib << 8;
    }
    return id;
}

std::string legacy_name(int id) {
    if (id == 0) return "minecraft:air";
    // Le C# rendait « minecraft:unknown_<id> ». On garde un nom stable et
    // explicite : la table numerique -> nom de la 1.12 n'est pas embarquee
    // (250 entrees), la coloration de carte se fait par identifiant.
    return "minecraft:id_" + std::to_string(id);
}

int index_of(int x, int y, int z) { return (y << 8) | (z << 4) | x; }

} // namespace

std::optional<File> parse_region_name(const fs::path& p) {
    // r.<x>.<z>.mca, x et z pouvant etre negatifs.
    const std::string n = p.filename().string();
    if (n.size() < 8 || n.compare(0, 2, "r.") != 0) return std::nullopt;
    if (n.size() < 4 || n.compare(n.size() - 4, 4, ".mca") != 0) return std::nullopt;
    const std::string mid = n.substr(2, n.size() - 6);
    const auto dot = mid.find('.');
    if (dot == std::string::npos) return std::nullopt;
    const std::string sx = mid.substr(0, dot), sz = mid.substr(dot + 1);
    if (sx.empty() || sz.empty()) return std::nullopt;
    auto numeric = [](const std::string& s) {
        std::size_t i = (s[0] == '-') ? 1 : 0;
        if (i >= s.size()) return false;
        for (; i < s.size(); ++i)
            if (s[i] < '0' || s[i] > '9') return false;
        return true;
    };
    if (!numeric(sx) || !numeric(sz)) return std::nullopt;
    File f;
    f.path = p;
    f.rx = std::atoi(sx.c_str());
    f.rz = std::atoi(sz.c_str());
    return f;
}

std::vector<File> list_regions(const fs::path& worldDir) {
    std::vector<File> out;
    std::error_code ec;
    // …/region, ou directement le dossier s'il contient deja les .mca.
    fs::path dir = worldDir / "region";
    if (!fs::is_directory(dir, ec)) dir = worldDir;
    if (!fs::is_directory(dir, ec)) return out;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (!e.is_regular_file(ec)) continue;
        if (auto f = parse_region_name(e.path())) out.push_back(*f);
    }
    std::sort(out.begin(), out.end(), [](const File& a, const File& b) {
        return a.rz != b.rz ? a.rz < b.rz : a.rx < b.rx;
    });
    return out;
}

std::vector<ChunkInfo> list_chunks(const fs::path& regionFile) {
    std::vector<ChunkInfo> out;
    // Seul l'en-tete de 8 Ko est necessaire : lire le fichier entier serait
    // ruineux sur un gros monde (Greenfield : 139 regions, 743 Mo au total).
    std::error_code ec;
    const auto fileSize = static_cast<std::size_t>(fs::file_size(regionFile, ec));
    if (ec || fileSize < kHeader) return out;
    std::vector<std::uint8_t> data(kHeader);
    {
        std::ifstream in(regionFile, std::ios::binary);
        if (!in) return out;
        in.read(reinterpret_cast<char*>(data.data()),
                static_cast<std::streamsize>(kHeader));
        if (!in) return out;
    }
    const auto rf = parse_region_name(regionFile);
    const int rx = rf ? rf->rx : 0;
    const int rz = rf ? rf->rz : 0;

    for (int i = 0; i < 1024; ++i) {
        const std::uint32_t off = be24(&data[i * 4]);
        const std::uint32_t sectors = data[i * 4 + 3];
        if (off == 0 || sectors == 0) continue; // chunk absent
        // Offset au-dela du fichier : en-tete corrompu, on ignore l'entree.
        if (static_cast<std::size_t>(off) * kSector >= fileSize) continue;
        ChunkInfo c;
        c.localX = i % 32;
        c.localZ = i / 32;
        c.cx = rx * 32 + c.localX;
        c.cz = rz * 32 + c.localZ;
        c.sectors = sectors;
        c.timestamp = be32(&data[kSector + i * 4]);
        out.push_back(c);
    }
    return out;
}

std::optional<nbt::Compound> read_chunk_local(const fs::path& regionFile, int lx,
                                              int lz) {
    if (lx < 0 || lx > 31 || lz < 0 || lz > 31) return std::nullopt;
    const auto data = read_all(regionFile);
    if (data.size() < kHeader) return std::nullopt;

    const int i = lz * 32 + lx;
    const std::uint32_t off = be24(&data[i * 4]);
    if (off == 0) return std::nullopt;

    const std::size_t pos = static_cast<std::size_t>(off) * kSector;
    if (pos + 5 > data.size()) return std::nullopt;
    const std::uint32_t len = be32(&data[pos]);
    // len compte l'octet de compression ; il doit rester dans le fichier.
    if (len == 0 || pos + 4 + len > data.size()) return std::nullopt;

    const std::uint8_t scheme = data[pos + 4];
    const std::uint8_t* payload = &data[pos + 5];
    const std::size_t plen = len - 1;
    if (plen == 0) return std::nullopt;

    // Le C# supposait toujours zlib ; gzip et « non compresse » sont legaux.
    switch (scheme) {
    case 1: return nbt::parse_gzip(payload, plen);
    case 2: return nbt::parse_zlib(payload, plen);
    case 3: return nbt::parse(payload, plen);
    default: return nbt::parse_auto(payload, plen);
    }
}

std::optional<nbt::Compound> read_chunk(const fs::path& worldDir, int cx, int cz) {
    // Division plancher : -1 / 32 doit donner -1, pas 0.
    const int rx = static_cast<int>(std::floor(cx / 32.0));
    const int rz = static_cast<int>(std::floor(cz / 32.0));
    std::error_code ec;
    fs::path dir = worldDir / "region";
    if (!fs::is_directory(dir, ec)) dir = worldDir;
    const fs::path f = dir / ("r." + std::to_string(rx) + "." + std::to_string(rz) +
                              ".mca");
    if (!fs::exists(f, ec)) return std::nullopt;
    return read_chunk_local(f, cx - rx * 32, cz - rz * 32);
}

std::string block_at(const nbt::Compound& chunk, int x, int y, int z) {
    if (x < 0 || x > 15 || z < 0 || z > 15) return {};
    const nbt::List* secs = sections_of(chunk);
    if (!secs) return {};
    const std::int64_t dv = data_version(chunk);
    const int wantY = static_cast<int>(std::floor(y / 16.0));
    const int localY = y - wantY * 16;

    for (const auto& t : *secs) {
        if (t.type != nbt::Type::Compound || !t.comp) continue;
        const Section s = decode_section(*t.comp, dv);
        if (!s.valid || s.y != wantY) continue;
        const int i = index_of(x, localY, z);
        if (s.bits < 0) return legacy_name(legacy_block_id(*t.comp, i));
        if (s.palette.empty()) return {};
        if (s.single) return s.palette[0];
        const int pi = palette_index(s.data, i, s.bits, s.packed);
        if (pi < 0 || static_cast<std::size_t>(pi) >= s.palette.size()) return {};
        return s.palette[static_cast<std::size_t>(pi)];
    }
    return {};
}

std::vector<std::int32_t> heightmap(const nbt::Compound& chunk) {
    std::vector<std::int32_t> hm(256, INT32_MIN);
    const nbt::List* secs = sections_of(chunk);
    if (!secs) return hm;
    const std::int64_t dv = data_version(chunk);

    // Sections decodees une seule fois, parcourues de haut en bas : des qu'une
    // colonne est remplie on ne la touche plus. Le C# materialisait tous les
    // blocs du chunk puis sondait 384 hauteurs par colonne.
    struct Entry {
        const nbt::Compound* comp;
        Section sec;
    };
    std::vector<Entry> list;
    for (const auto& t : *secs) {
        if (t.type != nbt::Type::Compound || !t.comp) continue;
        Section s = decode_section(*t.comp, dv);
        if (s.valid) list.push_back({t.comp.get(), std::move(s)});
    }
    std::sort(list.begin(), list.end(),
              [](const Entry& a, const Entry& b) { return a.sec.y > b.sec.y; });

    int remaining = 256;
    for (const Entry& e : list) {
        if (remaining == 0) break;
        const Section& s = e.sec;
        // Section entierement d'air : rien a y trouver.
        if (s.bits >= 0 && s.single && !s.palette.empty() &&
            s.palette[0] == "minecraft:air")
            continue;
        for (int ly = 15; ly >= 0 && remaining > 0; --ly) {
            for (int z = 0; z < 16; ++z) {
                for (int x = 0; x < 16; ++x) {
                    const int col = z * 16 + x;
                    if (hm[col] != INT32_MIN) continue;
                    const int i = index_of(x, ly, z);
                    std::string name;
                    if (s.bits < 0) {
                        const int id = legacy_block_id(*e.comp, i);
                        if (id <= 0) continue; // 0 = air, -1 = illisible
                        name = legacy_name(id);
                    } else if (s.palette.empty()) {
                        continue;
                    } else if (s.single) {
                        name = s.palette[0];
                    } else {
                        const int pi = palette_index(s.data, i, s.bits, s.packed);
                        if (pi < 0 || static_cast<std::size_t>(pi) >= s.palette.size())
                            continue;
                        name = s.palette[static_cast<std::size_t>(pi)];
                    }
                    if (name.empty() || name == "minecraft:air") continue;
                    hm[col] = s.y * 16 + ly;
                    --remaining;
                }
            }
        }
    }
    return hm;
}

std::vector<std::string> unique_blocks(const nbt::Compound& chunk) {
    std::set<std::string> seen;
    const nbt::List* secs = sections_of(chunk);
    if (!secs) return {};
    const std::int64_t dv = data_version(chunk);
    for (const auto& t : *secs) {
        if (t.type != nbt::Type::Compound || !t.comp) continue;
        const Section s = decode_section(*t.comp, dv);
        if (!s.valid) continue;
        if (s.bits < 0) {
            // Ancien format : on parcourt les identifiants presents.
            for (int i = 0; i < 4096; ++i) {
                const int id = legacy_block_id(*t.comp, i);
                if (id >= 0) seen.insert(legacy_name(id));
            }
            continue;
        }
        // Palette : elle contient deja exactement les blocs de la section.
        for (const auto& n : s.palette) seen.insert(n);
    }
    return {seen.begin(), seen.end()};
}

int clear_chunks(const fs::path& regionFile,
                 const std::vector<std::pair<int, int>>& localCoords) {
    auto data = read_all(regionFile);
    if (data.size() < kHeader) return 0;
    int n = 0;
    for (const auto& [lx, lz] : localCoords) {
        if (lx < 0 || lx > 31 || lz < 0 || lz > 31) continue;
        const int i = lz * 32 + lx;
        if (be24(&data[i * 4]) == 0 && data[i * 4 + 3] == 0) continue; // deja vide
        std::memset(&data[i * 4], 0, 4);            // offset + nb de secteurs
        std::memset(&data[kSector + i * 4], 0, 4);  // horodatage
        ++n;
    }
    if (n == 0) return 0;
    std::ofstream out(regionFile, std::ios::binary | std::ios::in);
    if (!out) return 0;
    out.seekp(0);
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(kHeader));
    return out ? n : 0;
}

} // namespace tl::region

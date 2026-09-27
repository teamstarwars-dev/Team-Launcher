#include "worldedit.hpp"

#include "nbt.hpp"
#include "region.hpp"

#include <algorithm>
#include <cmath>
#include <system_error>

namespace fs = std::filesystem;

namespace tl::worldedit {

namespace {

int chunk_of(int block) { return static_cast<int>(std::floor(block / 16.0)); }
int local_of(int block) { return ((block % 16) + 16) % 16; }

// ATTENTION : copier un `nbt::Compound` ne le duplique PAS en profondeur —
// `Tag` porte des `shared_ptr` vers ses compounds et listes filles, donc une
// copie partage tout le sous-arbre. Un « avant/apres » obtenu par affectation
// serait modifie en meme temps que l'original. L'etat d'origine est donc
// SERIALISE avant toute modification : c'est aussi la forme exacte qu'il faut
// dans l'instantane.
std::vector<std::uint8_t> freeze(const nbt::Compound& chunk) {
    return nbt::write_zlib(chunk);
}

// Ecrit un chunk et note son etat d'origine (deja serialise) dans l'instantane.
bool commit(const fs::path& worldDir, int cx, int cz,
            std::vector<std::uint8_t>&& before, const nbt::Compound& after,
            Snapshot* snap) {
    if (snap) {
        snap->bytes += before.size();
        snap->chunks.emplace(std::make_pair(cx, cz), std::move(before));
    }
    return region::write_chunk(worldDir, cx, cz, after);
}

// Verifie le monde et le volume demande. Renvoie un message d'erreur, ou "".
std::string precheck(const fs::path& worldDir, const Bounds& b) {
    std::error_code ec;
    if (!fs::is_directory(worldDir / "region", ec))
        return "Le dossier region/ du monde est introuvable.";
    const std::int64_t v = b.volume();
    if (v <= 0) return "Selection vide.";
    if (v > kMaxVolume)
        return "Selection trop grande (" + std::to_string(v) + " blocs, maximum " +
               std::to_string(kMaxVolume) + "). Reduis pos1/pos2.";
    return {};
}

// Parcourt les chunks couverts par le cuboide. `body` recoit le chunk lu et
// l'intersection en coordonnees monde ; il renvoie true si le chunk a change.
template <typename Body>
OpResult for_each_chunk(const fs::path& worldDir, const Bounds& b,
                        Snapshot* snap, const Progress& progress,
                        const std::atomic<bool>* cancel, Body&& body) {
    OpResult r;
    const int cx0 = chunk_of(b.x1), cx1 = chunk_of(b.x2);
    const int cz0 = chunk_of(b.z1), cz1 = chunk_of(b.z2);
    const int total = (cx1 - cx0 + 1) * (cz1 - cz0 + 1);
    int seen = 0;

    for (int cz = cz0; cz <= cz1; ++cz) {
        for (int cx = cx0; cx <= cx1; ++cx) {
            if (cancel && cancel->load()) {
                r.error = "Operation annulee.";
                return r;
            }
            ++seen;
            if (progress && (seen % 16 == 0 || seen == total))
                progress("Chunk " + std::to_string(seen) + "/" +
                         std::to_string(total));

            auto chunk = region::read_chunk(worldDir, cx, cz);
            if (!chunk) {
                // Chunk jamais genere : on ne le fabrique pas.
                const int nx = (std::min)(b.x2, cx * 16 + 15) -
                               (std::max)(b.x1, cx * 16) + 1;
                const int nz = (std::min)(b.z2, cz * 16 + 15) -
                               (std::max)(b.z1, cz * 16) + 1;
                r.skipped += nx * nz * (b.y2 - b.y1 + 1);
                continue;
            }
            // Fige l'etat d'origine AVANT modification (voir freeze()).
            std::vector<std::uint8_t> before;
            if (snap) before = freeze(*chunk);

            const int x0 = (std::max)(b.x1, cx * 16);
            const int x1 = (std::min)(b.x2, cx * 16 + 15);
            const int z0 = (std::max)(b.z1, cz * 16);
            const int z1 = (std::min)(b.z2, cz * 16 + 15);

            if (!body(*chunk, cx, cz, x0, x1, z0, z1, r)) continue;
            if (!r.error.empty()) return r;

            if (!commit(worldDir, cx, cz, std::move(before), *chunk, snap)) {
                r.error = "Echec d'ecriture du chunk (" + std::to_string(cx) +
                          ", " + std::to_string(cz) + ").";
                return r;
            }
            ++r.chunks;
        }
    }
    return r;
}

// Reecrit les chunks d'un instantane, en retenant leur etat courant.
OpResult restore(const fs::path& worldDir, const Snapshot& s, Snapshot& inverse) {
    OpResult r;
    for (const auto& [key, bytes] : s.chunks) {
        const auto [cx, cz] = key;
        if (auto cur = region::read_chunk(worldDir, cx, cz)) {
            auto raw = nbt::write_zlib(*cur);
            inverse.bytes += raw.size();
            inverse.chunks.emplace(key, std::move(raw));
        }
        auto old = nbt::parse_zlib(bytes.data(), bytes.size());
        if (!old) {
            r.error = "Instantane illisible pour le chunk (" +
                      std::to_string(cx) + ", " + std::to_string(cz) + ").";
            return r;
        }
        if (!region::write_chunk(worldDir, cx, cz, *old)) {
            r.error = "Echec de restauration du chunk (" + std::to_string(cx) +
                      ", " + std::to_string(cz) + ").";
            return r;
        }
        ++r.chunks;
    }
    return r;
}

} // namespace

std::int64_t Bounds::volume() const {
    const std::int64_t dx = static_cast<std::int64_t>(x2) - x1 + 1;
    const std::int64_t dy = static_cast<std::int64_t>(y2) - y1 + 1;
    const std::int64_t dz = static_cast<std::int64_t>(z2) - z1 + 1;
    if (dx <= 0 || dy <= 0 || dz <= 0) return 0;
    return dx * dy * dz;
}

Bounds bounds_of(const Pos& a, const Pos& b) {
    Bounds r;
    r.x1 = (std::min)(a.x, b.x);
    r.x2 = (std::max)(a.x, b.x);
    r.y1 = (std::min)(a.y, b.y);
    r.y2 = (std::max)(a.y, b.y);
    r.z1 = (std::min)(a.z, b.z);
    r.z2 = (std::max)(a.z, b.z);
    return r;
}

// --- History ----------------------------------------------------------------

std::size_t History::bytes() const {
    std::size_t n = 0;
    for (const auto& s : undo_) n += s.bytes;
    for (const auto& s : redo_) n += s.bytes;
    return n;
}

void History::trim(std::vector<Snapshot>& v) {
    while (v.size() > kMaxSteps) v.erase(v.begin());
    // Budget global : on sacrifie les etapes les plus anciennes de la pile
    // d'annulation, puis celles de la pile inverse.
    while (bytes() > kMaxBytes && !undo_.empty() && undo_.size() > 1)
        undo_.erase(undo_.begin());
    while (bytes() > kMaxBytes && !redo_.empty()) redo_.erase(redo_.begin());
}

void History::push_undo(Snapshot s) {
    if (s.chunks.empty()) return;
    undo_.push_back(std::move(s));
    redo_.clear(); // une nouvelle operation invalide le retablissement
    trim(undo_);
}

void History::clear() {
    undo_.clear();
    redo_.clear();
}

OpResult History::undo(const fs::path& worldDir) {
    OpResult r;
    if (undo_.empty()) {
        r.error = "Rien a annuler.";
        return r;
    }
    Snapshot inverse;
    r = restore(worldDir, undo_.back(), inverse);
    if (!r.error.empty()) return r;
    undo_.pop_back();
    if (!inverse.chunks.empty()) {
        redo_.push_back(std::move(inverse));
        trim(redo_);
    }
    return r;
}

OpResult History::redo(const fs::path& worldDir) {
    OpResult r;
    if (redo_.empty()) {
        r.error = "Rien a retablir.";
        return r;
    }
    Snapshot inverse;
    r = restore(worldDir, redo_.back(), inverse);
    if (!r.error.empty()) return r;
    redo_.pop_back();
    if (!inverse.chunks.empty()) {
        undo_.push_back(std::move(inverse));
        // Volontairement sans trim() de redo_ ici : l'etape vient d'en sortir.
        while (undo_.size() > kMaxSteps) undo_.erase(undo_.begin());
    }
    return r;
}

// --- Operations -------------------------------------------------------------

OpResult set_region(const fs::path& worldDir, const Bounds& b,
                    const std::string& name, History* hist,
                    const Progress& progress, const std::atomic<bool>* cancel) {
    OpResult r;
    if (name.empty()) {
        r.error = "Aucun bloc indique.";
        return r;
    }
    if (r.error = precheck(worldDir, b); !r.error.empty()) return r;

    Snapshot snap;
    r = for_each_chunk(
        worldDir, b, hist ? &snap : nullptr, progress, cancel,
        [&](nbt::Compound& chunk, int, int, int x0, int x1, int z0, int z1,
            OpResult& out) {
            std::vector<region::BlockEdit> edits;
            edits.reserve(static_cast<std::size_t>(x1 - x0 + 1) *
                          (z1 - z0 + 1) * (b.y2 - b.y1 + 1));
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z)
                    for (int y = b.y1; y <= b.y2; ++y)
                        edits.push_back({local_of(x), y, local_of(z), name});
            const auto er = region::set_blocks(chunk, edits);
            out.changed += er.applied;
            out.skipped += er.skipped;
            out.unsupported += er.unsupported;
            return er.applied > 0;
        });
    if (r.error.empty() && hist) hist->push_undo(std::move(snap));
    return r;
}

OpResult replace_region(const fs::path& worldDir, const Bounds& b,
                        const std::string& from, const std::string& to,
                        History* hist, const Progress& progress,
                        const std::atomic<bool>* cancel) {
    OpResult r;
    if (to.empty()) {
        r.error = "Aucun bloc de remplacement indique.";
        return r;
    }
    if (r.error = precheck(worldDir, b); !r.error.empty()) return r;
    const bool any = (from == "*");

    Snapshot snap;
    r = for_each_chunk(
        worldDir, b, hist ? &snap : nullptr, progress, cancel,
        [&](nbt::Compound& chunk, int, int, int x0, int x1, int z0, int z1,
            OpResult& out) {
            std::vector<region::BlockEdit> edits;
            {
                // Vue decodee : une passe de decodage par chunk, pas par bloc.
                const region::ChunkView view(chunk);
                for (int x = x0; x <= x1; ++x)
                    for (int z = z0; z <= z1; ++z)
                        for (int y = b.y1; y <= b.y2; ++y) {
                            const std::string cur =
                                view.at(local_of(x), y, local_of(z));
                            if (cur.empty()) continue; // section absente
                            if (cur == to) continue;
                            if (!any && cur != from) continue;
                            edits.push_back({local_of(x), y, local_of(z), to});
                        }
            }
            if (edits.empty()) return false;
            const auto er = region::set_blocks(chunk, edits);
            out.changed += er.applied;
            out.skipped += er.skipped;
            out.unsupported += er.unsupported;
            return er.applied > 0;
        });
    if (r.error.empty() && hist) hist->push_undo(std::move(snap));
    return r;
}

OpResult copy_region(const fs::path& worldDir, const Bounds& b, Clipboard& out,
                     const Progress& progress, const std::atomic<bool>* cancel) {
    OpResult r;
    if (r.error = precheck(worldDir, b); !r.error.empty()) return r;
    out = Clipboard{};
    out.sizeX = b.x2 - b.x1 + 1;
    out.sizeY = b.y2 - b.y1 + 1;
    out.sizeZ = b.z2 - b.z1 + 1;

    r = for_each_chunk(
        worldDir, b, nullptr, progress, cancel,
        [&](nbt::Compound& chunk, int, int, int x0, int x1, int z0, int z1,
            OpResult& res) {
            const region::ChunkView view(chunk);
            for (int x = x0; x <= x1; ++x)
                for (int z = z0; z <= z1; ++z)
                    for (int y = b.y1; y <= b.y2; ++y) {
                        const std::string n = view.at(local_of(x), y, local_of(z));
                        if (n.empty() || n == "minecraft:air") continue;
                        // Coordonnees RELATIVES au coin minimal (correctif 4).
                        out.blocks.push_back(
                            {Pos{x - b.x1, y - b.y1, z - b.z1}, n});
                        ++res.changed;
                    }
            return false; // lecture seule : rien a reecrire
        });
    if (!r.error.empty()) out = Clipboard{};
    return r;
}

OpResult paste(const fs::path& worldDir, const Clipboard& clip, const Pos& origin,
               History* hist, const Progress& progress,
               const std::atomic<bool>* cancel) {
    OpResult r;
    if (clip.empty()) {
        r.error = "Presse-papier vide : fais d'abord une copie.";
        return r;
    }
    Bounds b;
    b.x1 = origin.x;
    b.y1 = origin.y;
    b.z1 = origin.z;
    b.x2 = origin.x + clip.sizeX - 1;
    b.y2 = origin.y + clip.sizeY - 1;
    b.z2 = origin.z + clip.sizeZ - 1;
    if (r.error = precheck(worldDir, b); !r.error.empty()) return r;

    // Regroupement par chunk : un seul passage par chunk, comme partout
    // ailleurs (le C# reecrivait le fichier de region pour chaque bloc).
    std::map<std::pair<int, int>, std::vector<region::BlockEdit>> byChunk;
    for (const auto& [p, name] : clip.blocks) {
        const int wx = origin.x + p.x, wy = origin.y + p.y, wz = origin.z + p.z;
        byChunk[{chunk_of(wx), chunk_of(wz)}].push_back(
            {local_of(wx), wy, local_of(wz), name});
    }

    Snapshot snap;
    int seen = 0;
    for (const auto& [key, edits] : byChunk) {
        if (cancel && cancel->load()) {
            r.error = "Operation annulee.";
            return r;
        }
        ++seen;
        if (progress)
            progress("Chunk " + std::to_string(seen) + "/" +
                     std::to_string(byChunk.size()));
        const auto [cx, cz] = key;
        auto chunk = region::read_chunk(worldDir, cx, cz);
        if (!chunk) {
            r.skipped += static_cast<int>(edits.size());
            continue;
        }
        std::vector<std::uint8_t> before;
        if (hist) before = freeze(*chunk);
        const auto er = region::set_blocks(*chunk, edits);
        r.changed += er.applied;
        r.skipped += er.skipped;
        r.unsupported += er.unsupported;
        if (er.applied == 0) continue;
        if (!commit(worldDir, cx, cz, std::move(before), *chunk,
                    hist ? &snap : nullptr)) {
            r.error = "Echec d'ecriture du chunk (" + std::to_string(cx) + ", " +
                      std::to_string(cz) + ").";
            return r;
        }
        ++r.chunks;
    }
    if (hist) hist->push_undo(std::move(snap));
    return r;
}

} // namespace tl::worldedit

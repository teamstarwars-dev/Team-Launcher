#pragma once

// Lecture des regions Minecraft (.mca) — portage de ChunkReader.cs.
//
// Format : 8 Ko d'en-tete (1024 offsets de 4 octets + 1024 horodatages), puis
// les chunks alignes sur des secteurs de 4 Ko. Chaque chunk est prefixe de sa
// longueur (4 octets gros-boutiste) et d'un octet de compression
// (1 = gzip, 2 = zlib, 3 = non compresse).
//
// CORRECTIFS vs C# (ChunkReader.cs) :
//  1. `Y` de section est un TAG_Byte SIGNE (−4 a 19 en 1.18+). Le C# le lisait
//     en `byte` non signe : une section a −1 devenait 255, soit y = 4080.
//  2. Le format 1.13–1.17 (`BlockStates` + `Palette`) n'etait pas gere du tout.
//     Avant la 1.16 les entrees CHEVAUCHENT les long ; a partir de la 1.16
//     chaque long est rembourre. Les deux depaquetages sont implementes, le
//     choix se fait sur `DataVersion`.
//  3. Le C# ne lisait que l'octet de compression zlib et ignorait gzip et
//     « non compresse », pourtant legaux.
//  4. `GetHeightmap` du C# reconstruisait TOUS les blocs du chunk (jusqu'a
//     ~98 000 chaines) puis sondait 384 hauteurs par colonne. Ici la hauteur
//     est calculee par section, de haut en bas, sans materialiser les blocs.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "nbt.hpp"

namespace tl::region {

// Un fichier r.<x>.<z>.mca.
struct File {
    std::filesystem::path path;
    int rx = 0;
    int rz = 0;
};

// Chunk present dans une region.
struct ChunkInfo {
    int cx = 0;          // coordonnees monde (chunk)
    int cz = 0;
    int localX = 0;      // 0-31 dans la region
    int localZ = 0;
    std::uint32_t sectors = 0;   // taille en secteurs de 4 Ko
    std::int64_t timestamp = 0;  // epoch s. de derniere ecriture
};

// r.<x>.<z>.mca d'un dossier de monde (…/region). Trie par (rz, rx).
std::vector<File> list_regions(const std::filesystem::path& worldDir);

// Coordonnees d'apres le nom de fichier. nullopt si le nom ne correspond pas.
std::optional<File> parse_region_name(const std::filesystem::path& p);

// Chunks reellement presents dans la region.
std::vector<ChunkInfo> list_chunks(const std::filesystem::path& regionFile);

// NBT d'un chunk par coordonnees locales (0-31). nullopt = absent ou illisible.
std::optional<nbt::Compound> read_chunk_local(const std::filesystem::path& regionFile,
                                              int lx, int lz);

// NBT d'un chunk par coordonnees monde, en ouvrant la bonne region.
std::optional<nbt::Compound> read_chunk(const std::filesystem::path& worldDir,
                                        int cx, int cz);

// --- Contenu d'un chunk ---

// Nom du bloc en (x, y, z) local au chunk (x,z de 0 a 15). "" si inconnu.
// Prend en charge 1.18+, 1.13–1.17 et <= 1.12.
std::string block_at(const nbt::Compound& chunk, int x, int y, int z);

// Hauteur du plus haut bloc non-air de chaque colonne, indexe [z * 16 + x].
// Valeur INT32_MIN pour une colonne entierement vide.
std::vector<std::int32_t> heightmap(const nbt::Compound& chunk);

// Noms de blocs distincts du chunk (coloration de carte).
std::vector<std::string> unique_blocks(const nbt::Compound& chunk);

// --- Ecriture ---

// Marque des chunks comme absents (offset et horodatage a zero) : c'est ce que
// fait « supprimer la selection » de l'editeur de cartes. Les secteurs ne sont
// pas recuperes, Minecraft les reutilisera. Retourne le nombre efface.
int clear_chunks(const std::filesystem::path& regionFile,
                 const std::vector<std::pair<int, int>>& localCoords);

// Ecrit (ou remplace) un chunk dans une region, coordonnees locales 0-31.
// Le fichier est cree s'il n'existe pas. Le chunk est compresse en zlib
// (schema 2, celui de Minecraft) et place dans des secteurs libres ; les
// anciens secteurs ne sont pas recuperes, Minecraft les reutilisera.
//
// L'ecriture se fait dans un fichier temporaire puis remplacement atomique :
// une coupure en cours d'ecriture ne laisse jamais une region a moitie ecrite
// (le C#, lui, ecrasait le fichier en place).
bool write_chunk_local(const std::filesystem::path& regionFile, int lx, int lz,
                       const nbt::Compound& chunk);

// Meme chose par coordonnees monde, en creant la region au besoin.
bool write_chunk(const std::filesystem::path& worldDir, int cx, int cz,
                 const nbt::Compound& chunk);

// --- Modification des blocs d'un chunk ---

struct BlockEdit {
    int x = 0;  // 0-15, local au chunk
    int y = 0;  // altitude monde
    int z = 0;  // 0-15
    std::string name; // « minecraft:stone »
};

// Applique des changements de blocs a un chunk deja lu, en place. Les sections
// concernees sont re-encodees (palette + indices reconstruits).
//
// Formats pris en charge :
//   - 1.18+       : block_states { palette, data }, indices non chevauchants
//   - 1.13–1.17   : Palette + BlockStates, indices tasses avant la 1.16
//   - <= 1.12     : Blocks + Data, identifiants numeriques — seuls les noms
//                   presents dans la table interne sont acceptes (ceux du
//                   generateur de ville), les autres sont comptes en echec.
//
// Retourne le nombre de blocs effectivement poses. Les sections absentes ne
// sont pas creees : poser un bloc dans le vide est ignore (`skipped`).
struct EditResult {
    int applied = 0;
    int skipped = 0;   // hors chunk, ou section absente
    int unsupported = 0; // nom de bloc inconnu en <= 1.12
};
EditResult set_blocks(nbt::Compound& chunk, const std::vector<BlockEdit>& edits);

// Identifiant numerique 1.12 d'un nom de bloc, -1 si inconnu.
int legacy_id_for(const std::string& name);

} // namespace tl::region

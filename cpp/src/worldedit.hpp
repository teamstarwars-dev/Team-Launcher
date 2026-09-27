#pragma once

// Operations WorldEdit sur un monde Minecraft — portage de la partie
// « WorldEdit » d'EditorCanvas.cs : selection cuboide (pos1/pos2), //set,
// //replace, //copy, //paste, //undo, //redo.
//
// CORRECTIFS vs C# (EditorCanvas.cs) — l'implementation d'origine etait
// inutilisable en pratique :
//
//  1. `SetBlock` RELISAIT ET REECRIVAIT le chunk entier pour CHAQUE bloc. Un
//     cuboide de 16x16x16 declenchait 4 096 lectures de chunk et 4 096
//     reecritures completes du fichier de region (plusieurs Mo chacune). Ici
//     les blocs sont regroupes par chunk : une lecture, une ecriture.
//  2. `SaveUndo` copiait TOUS les fichiers de region du monde en memoire, et
//     gardait jusqu'a 20 instantanes. Sur un monde de 700 Mo cela demandait
//     14 Go de RAM. Ici seuls les CHUNKS TOUCHES sont sauvegardes, sous forme
//     NBT compressee, avec un budget memoire explicite.
//  3. `GetBlock` materialisait le dictionnaire de tous les blocs du chunk
//     (~98 000 entrees) pour lire une seule position. Ici une `ChunkView`
//     decode les sections une fois par chunk.
//  4. `PasteAsync` calculait le decalage a partir du PREMIER bloc du
//     presse-papier et non du coin minimal de la selection. Comme l'air est
//     exclu de la copie, un coin minimal vide decalait tout le collage. Le
//     presse-papier est ici stocke en coordonnees relatives au coin minimal.
//  5. Aucune borne sur le volume : une selection de 10 000 blocs de cote
//     lancait 10^12 iterations. Un plafond est applique.
//  6. Toutes les erreurs etaient avalees par des `catch {}` silencieux.

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace tl::worldedit {

struct Pos {
    int x = 0, y = 0, z = 0;
};

// Cuboide inclusif, deja normalise (x1 <= x2, etc.).
struct Bounds {
    int x1 = 0, y1 = 0, z1 = 0;
    int x2 = 0, y2 = 0, z2 = 0;
    std::int64_t volume() const;
};

// Normalise deux coins en cuboide inclusif.
Bounds bounds_of(const Pos& a, const Pos& b);

// Plafond de volume d'une operation. 8 millions de blocs, soit un cube de 200
// de cote : au-dela l'operation est refusee plutot que lancee pour des heures.
inline constexpr std::int64_t kMaxVolume = 8'000'000;

struct OpResult {
    int changed = 0;      // blocs effectivement modifies
    int skipped = 0;      // chunk ou section absents
    int unsupported = 0;  // nom sans equivalent dans un monde <= 1.12
    int chunks = 0;       // chunks reecrits
    std::string error;    // vide si tout s'est bien passe
};

// --- Historique -------------------------------------------------------------

// Instantane des chunks touches par une operation, en NBT compresse. Pas de
// copie des fichiers de region : seuls les chunks modifies sont retenus.
struct Snapshot {
    // cle : (cx, cz) en coordonnees monde ; valeur : chunk NBT compresse zlib.
    std::map<std::pair<int, int>, std::vector<std::uint8_t>> chunks;
    std::size_t bytes = 0;
};

// Pile d'annulation bornee : au plus `kMaxSteps` etapes ET `kMaxBytes` au
// total. Les etapes les plus anciennes sont abandonnees en premier.
class History {
public:
    static constexpr std::size_t kMaxSteps = 20;
    static constexpr std::size_t kMaxBytes = 128u * 1024 * 1024;

    void push_undo(Snapshot s);
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    std::size_t undo_count() const { return undo_.size(); }
    std::size_t redo_count() const { return redo_.size(); }
    std::size_t bytes() const;
    void clear();

    // Restaure l'etat precedent ; l'etat courant des memes chunks part dans
    // la pile inverse. Renvoie une erreur si rien a faire.
    OpResult undo(const std::filesystem::path& worldDir);
    OpResult redo(const std::filesystem::path& worldDir);

private:
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    void trim(std::vector<Snapshot>& v);
};

// --- Presse-papier ----------------------------------------------------------

// Blocs copies, en coordonnees RELATIVES au coin minimal de la selection
// (correctif 4). L'air n'est pas copie, comme dans le C#.
struct Clipboard {
    int sizeX = 0, sizeY = 0, sizeZ = 0;
    std::vector<std::pair<Pos, std::string>> blocks;
    bool empty() const { return blocks.empty(); }
};

using Progress = std::function<void(const std::string&)>;

// --- Operations -------------------------------------------------------------

// //set : remplit le cuboide d'un seul bloc.
OpResult set_region(const std::filesystem::path& worldDir, const Bounds& b,
                    const std::string& name, History* hist,
                    const Progress& progress = {},
                    const std::atomic<bool>* cancel = nullptr);

// //replace : remplace `from` par `to`. `from` vaut "*" pour tout remplacer
// (sauf ce qui est deja `to`).
OpResult replace_region(const std::filesystem::path& worldDir, const Bounds& b,
                        const std::string& from, const std::string& to,
                        History* hist, const Progress& progress = {},
                        const std::atomic<bool>* cancel = nullptr);

// //copy : remplit `out`. Ne modifie rien.
OpResult copy_region(const std::filesystem::path& worldDir, const Bounds& b,
                     Clipboard& out, const Progress& progress = {},
                     const std::atomic<bool>* cancel = nullptr);

// //paste : colle le presse-papier, son coin minimal place en `origin`.
OpResult paste(const std::filesystem::path& worldDir, const Clipboard& clip,
               const Pos& origin, History* hist, const Progress& progress = {},
               const std::atomic<bool>* cancel = nullptr);

} // namespace tl::worldedit

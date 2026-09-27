#pragma once

// Lecteur NBT (Named Binary Tag) — format des mondes Minecraft.
//
// CORRECTIF vs C# (NbtReader.cs) : le NBT est GROS-BOUTISTE (format Java),
// alors que `BinaryReader` de .NET lit en petit-boutiste sans compensation.
// Le lecteur C# inversait donc tous les entiers, y compris les longueurs de
// noms : « 00 04 » y valait 1024 au lieu de 4, et l'analyse partait aussitot
// hors des rails. ChunkReader.cs faisait pourtant bien le gros-boutiste a la
// main pour l'en-tete de region avant d'appeler NbtReader. Ce port lit le
// format correctement.
//
// Robustesse : ces donnees viennent de mondes telecharges ou partages. Toutes
// les lectures sont bornees, la profondeur d'imbrication est limitee et les
// tailles de tableaux sont controlees avant allocation — un fichier corrompu
// ou hostile donne `nullopt`, jamais un depassement ni une allocation folle.

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tl::nbt {

enum class Type : std::uint8_t {
    End = 0, Byte, Short, Int, Long, Float, Double,
    ByteArray, String, List, Compound, IntArray, LongArray
};

struct Tag;
using Compound = std::map<std::string, Tag>;
using List = std::vector<Tag>;

struct Tag {
    Type type = Type::End;
    std::int64_t num = 0;  // Byte, Short, Int, Long
    double dbl = 0.0;      // Float, Double
    std::string str;       // String
    std::vector<std::uint8_t> bytes;
    std::vector<std::int32_t> ints;
    std::vector<std::int64_t> longs;
    std::shared_ptr<Compound> comp;
    std::shared_ptr<List> list;
    Type listElem = Type::End;

    bool is(Type t) const { return type == t; }
};

// Profondeur maximale d'imbrication (les mondes reels depassent rarement 10).
inline constexpr int kMaxDepth = 64;

// --- Analyse ---
// Les trois formes rencontrees : brut, gzip (level.dat) et zlib (chunks .mca).
std::optional<Compound> parse(const std::uint8_t* data, std::size_t n);
std::optional<Compound> parse_gzip(const std::uint8_t* data, std::size_t n);
std::optional<Compound> parse_zlib(const std::uint8_t* data, std::size_t n);

// Detecte la compression d'apres les octets de tete (1f 8b = gzip,
// 78 = zlib) et applique la bonne analyse.
std::optional<Compound> parse_auto(const std::uint8_t* data, std::size_t n);
std::optional<Compound> read_file(const std::string& path);

// --- Ecriture ---
//
// Meme remarque que pour la lecture : le C# (CityGenerator.SerializeCompound)
// serialisait avec un `BinaryWriter` petit-boutiste, donc produisait des
// chunks que Minecraft ne peut pas relire. Ici tout est ecrit gros-boutiste.

std::vector<std::uint8_t> write(const Compound& root, const std::string& rootName = {});
std::vector<std::uint8_t> write_gzip(const Compound& root,
                                     const std::string& rootName = {});
std::vector<std::uint8_t> write_zlib(const Compound& root,
                                     const std::string& rootName = {});
bool write_file_gzip(const std::string& path, const Compound& root,
                     const std::string& rootName = {});

// --- Fabrication de tags ---
Tag make_byte(std::int8_t v);
Tag make_short(std::int16_t v);
Tag make_int(std::int32_t v);
Tag make_long(std::int64_t v);
Tag make_float(float v);
Tag make_double(double v);
Tag make_string(std::string v);
Tag make_byte_array(std::vector<std::uint8_t> v);
Tag make_int_array(std::vector<std::int32_t> v);
Tag make_long_array(std::vector<std::int64_t> v);
Tag make_compound(Compound v);
// elem doit correspondre au type des elements ; une liste vide est ecrite
// avec TAG_End comme type d'element, ce qu'attend Minecraft.
Tag make_list(Type elem, List v);

// --- Acces confortable (nullptr / valeur par defaut si absent ou mal type) ---
const Tag* find(const Compound& c, const std::string& key);
const Compound* get_compound(const Compound& c, const std::string& key);
const List* get_list(const Compound& c, const std::string& key);
std::int64_t get_num(const Compound& c, const std::string& key, std::int64_t def = 0);
double get_dbl(const Compound& c, const std::string& key, double def = 0.0);
std::string get_string(const Compound& c, const std::string& key,
                       const std::string& def = {});

// Nom lisible d'un type (diagnostic, affichage arborescent).
const char* type_name(Type t);

} // namespace tl::nbt

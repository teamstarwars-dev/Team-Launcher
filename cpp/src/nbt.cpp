#include "nbt.hpp"

#include "miniz.h"

#include <cstring>
#include <fstream>
#include <sstream>

namespace tl::nbt {

namespace {

// Limites de securite : un fichier corrompu ou hostile ne doit ni faire
// exploser la memoire ni boucler. Les valeurs sont tres au-dessus de ce
// qu'un monde reel produit (un chunk fait quelques dizaines de Ko).
constexpr std::size_t kMaxArray = 64u * 1024 * 1024;   // elements
constexpr std::size_t kMaxString = 1u << 20;           // octets
constexpr std::size_t kMaxInflated = 256u * 1024 * 1024;

// Lecteur borne, GROS-BOUTISTE (c'est tout l'objet du correctif).
class Reader {
public:
    Reader(const std::uint8_t* d, std::size_t n) : d_(d), n_(n) {}

    bool ok() const { return ok_; }
    std::size_t left() const { return ok_ ? n_ - p_ : 0; }

    std::uint8_t u8() {
        if (!need(1)) return 0;
        return d_[p_++];
    }
    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }

    std::int16_t i16() {
        if (!need(2)) return 0;
        const std::uint16_t v =
            static_cast<std::uint16_t>(d_[p_] << 8 | d_[p_ + 1]);
        p_ += 2;
        return static_cast<std::int16_t>(v);
    }
    std::uint16_t u16() { return static_cast<std::uint16_t>(i16()); }

    std::int32_t i32() {
        if (!need(4)) return 0;
        const std::uint32_t v = static_cast<std::uint32_t>(d_[p_]) << 24 |
                                static_cast<std::uint32_t>(d_[p_ + 1]) << 16 |
                                static_cast<std::uint32_t>(d_[p_ + 2]) << 8 |
                                static_cast<std::uint32_t>(d_[p_ + 3]);
        p_ += 4;
        return static_cast<std::int32_t>(v);
    }

    std::int64_t i64() {
        if (!need(8)) return 0;
        std::uint64_t v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 8) | d_[p_ + k];
        p_ += 8;
        return static_cast<std::int64_t>(v);
    }

    float f32() {
        const std::int32_t bits = i32();
        float f = 0.0f;
        std::memcpy(&f, &bits, 4);
        return f;
    }
    double f64() {
        const std::int64_t bits = i64();
        double v = 0.0;
        std::memcpy(&v, &bits, 8);
        return v;
    }

    // Chaine NBT : longueur sur 2 octets gros-boutiste, puis UTF-8 (modifie).
    std::string str() {
        const std::uint16_t len = u16();
        if (!ok_ || len > kMaxString || !need(len)) {
            fail();
            return {};
        }
        std::string s(reinterpret_cast<const char*>(d_ + p_), len);
        p_ += len;
        return s;
    }

    void fail() { ok_ = false; }

private:
    bool need(std::size_t k) {
        if (!ok_ || p_ + k > n_) {
            ok_ = false;
            return false;
        }
        return true;
    }
    const std::uint8_t* d_;
    std::size_t n_;
    std::size_t p_ = 0;
    bool ok_ = true;
};

bool valid_type(std::uint8_t t) { return t <= 12; }

Tag read_value(Reader& r, Type t, int depth);

Compound read_compound_body(Reader& r, int depth) {
    Compound c;
    for (;;) {
        const std::uint8_t tt = r.u8();
        if (!r.ok()) return c;
        if (tt == 0) return c; // TAG_End
        if (!valid_type(tt)) {
            r.fail();
            return c;
        }
        const std::string name = r.str();
        if (!r.ok()) return c;
        Tag v = read_value(r, static_cast<Type>(tt), depth + 1);
        if (!r.ok()) return c;
        c[name] = std::move(v);
    }
}

Tag read_value(Reader& r, Type t, int depth) {
    Tag tag;
    tag.type = t;
    if (depth > kMaxDepth) { // imbrication abusive
        r.fail();
        return tag;
    }
    switch (t) {
    case Type::Byte: tag.num = r.i8(); break;
    case Type::Short: tag.num = r.i16(); break;
    case Type::Int: tag.num = r.i32(); break;
    case Type::Long: tag.num = r.i64(); break;
    case Type::Float: tag.dbl = r.f32(); break;
    case Type::Double: tag.dbl = r.f64(); break;
    case Type::String: tag.str = r.str(); break;
    case Type::ByteArray: {
        const std::int32_t n = r.i32();
        if (n < 0 || static_cast<std::size_t>(n) > kMaxArray ||
            static_cast<std::size_t>(n) > r.left()) {
            r.fail();
            break;
        }
        tag.bytes.resize(static_cast<std::size_t>(n));
        for (auto& b : tag.bytes) b = r.u8();
        break;
    }
    case Type::IntArray: {
        const std::int32_t n = r.i32();
        // 4 octets par element : on verifie avant d'allouer.
        if (n < 0 || static_cast<std::size_t>(n) > kMaxArray ||
            static_cast<std::size_t>(n) * 4 > r.left()) {
            r.fail();
            break;
        }
        tag.ints.resize(static_cast<std::size_t>(n));
        for (auto& v : tag.ints) v = r.i32();
        break;
    }
    case Type::LongArray: {
        const std::int32_t n = r.i32();
        if (n < 0 || static_cast<std::size_t>(n) > kMaxArray ||
            static_cast<std::size_t>(n) * 8 > r.left()) {
            r.fail();
            break;
        }
        tag.longs.resize(static_cast<std::size_t>(n));
        for (auto& v : tag.longs) v = r.i64();
        break;
    }
    case Type::List: {
        const std::uint8_t et = r.u8();
        const std::int32_t n = r.i32();
        if (!valid_type(et) || n < 0 ||
            static_cast<std::size_t>(n) > kMaxArray) {
            r.fail();
            break;
        }
        tag.listElem = static_cast<Type>(et);
        tag.list = std::make_shared<List>();
        // TAG_End comme type d'element : liste vide (cas legal et frequent).
        if (et == 0) break;
        tag.list->reserve(static_cast<std::size_t>(n) < 4096
                              ? static_cast<std::size_t>(n)
                              : 4096);
        for (std::int32_t i = 0; i < n; ++i) {
            tag.list->push_back(read_value(r, static_cast<Type>(et), depth + 1));
            if (!r.ok()) break;
        }
        break;
    }
    case Type::Compound:
        tag.comp = std::make_shared<Compound>(read_compound_body(r, depth + 1));
        break;
    case Type::End:
    default:
        r.fail();
        break;
    }
    return tag;
}

// --- decompression (miniz) ---

std::optional<std::vector<std::uint8_t>> inflate_raw(const std::uint8_t* d,
                                                     std::size_t n, bool zlibHdr) {
    std::size_t outLen = 0;
    void* out = tinfl_decompress_mem_to_heap(
        d, n, &outLen, zlibHdr ? TINFL_FLAG_PARSE_ZLIB_HEADER : 0);
    if (!out) return std::nullopt;
    if (outLen > kMaxInflated) {
        mz_free(out);
        return std::nullopt;
    }
    std::vector<std::uint8_t> v(static_cast<const std::uint8_t*>(out),
                                static_cast<const std::uint8_t*>(out) + outLen);
    mz_free(out);
    return v;
}

// En-tete gzip (RFC 1952) : 10 octets fixes puis champs optionnels.
std::size_t gzip_header_size(const std::uint8_t* d, std::size_t n) {
    if (n < 18 || d[0] != 0x1F || d[1] != 0x8B || d[2] != 0x08) return 0;
    const std::uint8_t flg = d[3];
    std::size_t p = 10;
    if (flg & 0x04) { // FEXTRA
        if (p + 2 > n) return 0;
        const std::size_t xlen = static_cast<std::size_t>(d[p]) | (static_cast<std::size_t>(d[p + 1]) << 8);
        p += 2 + xlen;
    }
    if (flg & 0x08) { // FNAME
        while (p < n && d[p] != 0) ++p;
        ++p;
    }
    if (flg & 0x10) { // FCOMMENT
        while (p < n && d[p] != 0) ++p;
        ++p;
    }
    if (flg & 0x02) p += 2; // FHCRC
    return p < n ? p : 0;
}

} // namespace

std::optional<Compound> parse(const std::uint8_t* data, std::size_t n) {
    if (!data || n < 3) return std::nullopt;
    Reader r(data, n);
    if (r.u8() != 10) return std::nullopt; // racine = TAG_Compound
    r.str();                               // nom de racine (souvent vide)
    if (!r.ok()) return std::nullopt;
    Compound c = read_compound_body(r, 0);
    if (!r.ok()) return std::nullopt;
    return c;
}

std::optional<Compound> parse_gzip(const std::uint8_t* data, std::size_t n) {
    const std::size_t h = gzip_header_size(data, n);
    if (h == 0) return std::nullopt;
    auto raw = inflate_raw(data + h, n - h, /*zlibHdr=*/false);
    if (!raw) return std::nullopt;
    return parse(raw->data(), raw->size());
}

std::optional<Compound> parse_zlib(const std::uint8_t* data, std::size_t n) {
    auto raw = inflate_raw(data, n, /*zlibHdr=*/true);
    if (!raw) return std::nullopt;
    return parse(raw->data(), raw->size());
}

std::optional<Compound> parse_auto(const std::uint8_t* data, std::size_t n) {
    if (!data || n < 3) return std::nullopt;
    if (data[0] == 0x1F && data[1] == 0x8B) return parse_gzip(data, n);
    // zlib : CMF/FLG dont (CMF*256+FLG) % 31 == 0 et methode deflate.
    if ((data[0] & 0x0F) == 0x08 &&
        ((static_cast<unsigned>(data[0]) << 8 | data[1]) % 31) == 0)
        return parse_zlib(data, n);
    return parse(data, n);
}

std::optional<Compound> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string s = ss.str();
    if (s.empty()) return std::nullopt;
    return parse_auto(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
}

// --- accesseurs ---

const Tag* find(const Compound& c, const std::string& key) {
    const auto it = c.find(key);
    return it == c.end() ? nullptr : &it->second;
}

const Compound* get_compound(const Compound& c, const std::string& key) {
    const Tag* t = find(c, key);
    return (t && t->type == Type::Compound && t->comp) ? t->comp.get() : nullptr;
}

const List* get_list(const Compound& c, const std::string& key) {
    const Tag* t = find(c, key);
    return (t && t->type == Type::List && t->list) ? t->list.get() : nullptr;
}

std::int64_t get_num(const Compound& c, const std::string& key, std::int64_t def) {
    const Tag* t = find(c, key);
    if (!t) return def;
    switch (t->type) {
    case Type::Byte:
    case Type::Short:
    case Type::Int:
    case Type::Long: return t->num;
    case Type::Float:
    case Type::Double: return static_cast<std::int64_t>(t->dbl);
    default: return def;
    }
}

double get_dbl(const Compound& c, const std::string& key, double def) {
    const Tag* t = find(c, key);
    if (!t) return def;
    if (t->type == Type::Float || t->type == Type::Double) return t->dbl;
    if (t->type == Type::Byte || t->type == Type::Short || t->type == Type::Int ||
        t->type == Type::Long)
        return static_cast<double>(t->num);
    return def;
}

std::string get_string(const Compound& c, const std::string& key,
                       const std::string& def) {
    const Tag* t = find(c, key);
    return (t && t->type == Type::String) ? t->str : def;
}

const char* type_name(Type t) {
    switch (t) {
    case Type::End: return "End";
    case Type::Byte: return "Byte";
    case Type::Short: return "Short";
    case Type::Int: return "Int";
    case Type::Long: return "Long";
    case Type::Float: return "Float";
    case Type::Double: return "Double";
    case Type::ByteArray: return "ByteArray";
    case Type::String: return "String";
    case Type::List: return "List";
    case Type::Compound: return "Compound";
    case Type::IntArray: return "IntArray";
    case Type::LongArray: return "LongArray";
    }
    return "?";
}

} // namespace tl::nbt

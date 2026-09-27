// Tests du lecteur NBT : gros-boutiste, tous les types, compressions, et
// resistance aux fichiers corrompus ou hostiles.
//
// Rappel du correctif porte : NbtReader.cs lisait en petit-boutiste (le
// BinaryReader de .NET), donc une longueur de nom « 00 04 » y valait 1024.
// Les vecteurs ci-dessous sont ecrits a la main en gros-boutiste : s'ils
// passent, c'est que le format est lu correctement.

#include "nbt.hpp"

#include "miniz.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tl::nbt;

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const auto va = (a);                                                 \
        const auto vb = (b);                                                 \
        if (!(va == vb)) {                                                   \
            std::printf("FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// --- construction d'un flux NBT a la main (gros-boutiste) ------------------
struct Buf {
    std::vector<std::uint8_t> v;
    void u8(int b) { v.push_back(static_cast<std::uint8_t>(b)); }
    void i16(int x) { u8((x >> 8) & 0xFF); u8(x & 0xFF); }
    void i32(long long x) { for (int s = 24; s >= 0; s -= 8) u8((x >> s) & 0xFF); }
    void i64(long long x) { for (int s = 56; s >= 0; s -= 8) u8((x >> s) & 0xFF); }
    void str(const std::string& s) {
        i16(static_cast<int>(s.size()));
        for (char c : s) u8(static_cast<unsigned char>(c));
    }
    // en-tete d'un tag nomme
    void tag(int type, const std::string& name) { u8(type); str(name); }
};

static std::vector<std::uint8_t> zlib_compress(const std::vector<std::uint8_t>& in) {
    mz_ulong bound = mz_compressBound(static_cast<mz_ulong>(in.size()));
    std::vector<std::uint8_t> out(bound);
    if (mz_compress(out.data(), &bound, in.data(),
                    static_cast<mz_ulong>(in.size())) != MZ_OK)
        return {};
    out.resize(bound);
    return out;
}

// gzip = en-tete 10 octets + deflate brut + CRC32 + taille
static std::vector<std::uint8_t> gzip_compress(const std::vector<std::uint8_t>& in) {
    const auto z = zlib_compress(in);
    if (z.size() < 6) return {};
    // On retire l'en-tete zlib (2 octets) et l'adler32 final (4 octets).
    std::vector<std::uint8_t> out = {0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0xFF};
    out.insert(out.end(), z.begin() + 2, z.end() - 4);
    const mz_ulong crc = mz_crc32(MZ_CRC32_INIT, in.data(),
                                  static_cast<size_t>(in.size()));
    for (int s = 0; s < 32; s += 8) out.push_back((crc >> s) & 0xFF);
    const std::uint32_t n = static_cast<std::uint32_t>(in.size());
    for (int s = 0; s < 32; s += 8) out.push_back((n >> s) & 0xFF);
    return out;
}

int main() {
    // =====================================================================
    // 1. Tous les types, dans un compound racine
    // =====================================================================
    Buf b;
    b.u8(10);          // racine : TAG_Compound
    b.str("Data");     // nom de racine non vide (cas reel de level.dat)

    b.tag(1, "octet");   b.u8(0xFF);              // Byte = -1 (signe)
    b.tag(2, "court");   b.i16(0x0102);           // 258
    b.tag(3, "entier");  b.i32(123456789);
    b.tag(4, "long");    b.i64(1234567890123LL);
    b.tag(5, "flottant");                          // 1.5f = 0x3FC00000
    b.i32(0x3FC00000);
    b.tag(6, "double");                            // 2.5 = 0x4004000000000000
    b.i64(0x4004000000000000LL);
    b.tag(8, "texte");   b.str("bonjour");
    b.tag(7, "octets");  b.i32(3); b.u8(1); b.u8(2); b.u8(3);
    b.tag(11, "entiers"); b.i32(2); b.i32(10); b.i32(-20);
    b.tag(12, "longs");   b.i32(1); b.i64(-5);
    // Liste de compounds
    b.tag(9, "liste");   b.u8(10); b.i32(2);
    b.tag(3, "n"); b.i32(1); b.u8(0);              // {n:1}
    b.tag(3, "n"); b.i32(2); b.u8(0);              // {n:2}
    // Compound imbrique
    b.tag(10, "sous");
    b.tag(8, "cle"); b.str("valeur");
    b.u8(0);                                        // fin de "sous"
    // Liste vide declaree TAG_End (cas legal frequent)
    b.tag(9, "vide"); b.u8(0); b.i32(0);
    b.u8(0);                                        // fin de racine

    auto root = parse(b.v.data(), b.v.size());
    CHECK(root.has_value());
    if (root) {
        const Compound& c = *root;
        CHECK_EQ(get_num(c, "octet"), -1LL);        // signe conserve
        CHECK_EQ(get_num(c, "court"), 258LL);       // gros-boutiste
        CHECK_EQ(get_num(c, "entier"), 123456789LL);
        CHECK_EQ(get_num(c, "long"), 1234567890123LL);
        CHECK_EQ(get_dbl(c, "flottant"), 1.5);
        CHECK_EQ(get_dbl(c, "double"), 2.5);
        CHECK_EQ(get_string(c, "texte"), std::string("bonjour"));

        const Tag* oct = find(c, "octets");
        CHECK(oct && oct->bytes.size() == 3);
        if (oct && oct->bytes.size() == 3) CHECK_EQ(oct->bytes[2], 3);

        const Tag* ent = find(c, "entiers");
        CHECK(ent && ent->ints.size() == 2);
        if (ent && ent->ints.size() == 2) {
            CHECK_EQ(ent->ints[0], 10);
            CHECK_EQ(ent->ints[1], -20);
        }
        const Tag* lg = find(c, "longs");
        CHECK(lg && lg->longs.size() == 1);
        if (lg && lg->longs.size() == 1) CHECK_EQ(lg->longs[0], -5LL);

        const List* l = get_list(c, "liste");
        CHECK(l && l->size() == 2);
        if (l && l->size() == 2) {
            CHECK((*l)[0].comp && get_num(*(*l)[0].comp, "n") == 1);
            CHECK((*l)[1].comp && get_num(*(*l)[1].comp, "n") == 2);
        }
        const Compound* sub = get_compound(c, "sous");
        CHECK(sub != nullptr);
        if (sub) CHECK_EQ(get_string(*sub, "cle"), std::string("valeur"));

        const List* vide = get_list(c, "vide");
        CHECK(vide && vide->empty());

        // Accesseurs : cle absente ou mauvais type -> valeur par defaut
        CHECK_EQ(get_num(c, "absent", 42), 42LL);
        CHECK_EQ(get_string(c, "entier", "def"), std::string("def"));
        CHECK(get_compound(c, "texte") == nullptr);
        CHECK(get_list(c, "texte") == nullptr);
        CHECK(find(c, "absent") == nullptr);
    }

    // =====================================================================
    // 2. Piege du petit-boutiste : ce que l'on attrape ici
    // =====================================================================
    // « 00 07 » comme longueur de nom vaut 7 en gros-boutiste, 1792 en
    // petit-boutiste. Le lecteur C# partait donc hors des rails des le
    // premier tag nomme.
    {
        Buf p;
        p.u8(10); p.str("");
        p.tag(3, "monentr");  // nom de 7 octets
        p.i32(7);
        p.u8(0);
        auto r = parse(p.v.data(), p.v.size());
        CHECK(r.has_value());
        if (r) CHECK_EQ(get_num(*r, "monentr"), 7LL);
    }

    // =====================================================================
    // 3. Compressions : zlib (chunks .mca) et gzip (level.dat)
    // =====================================================================
    {
        const auto z = zlib_compress(b.v);
        CHECK(!z.empty());
        auto rz = parse_zlib(z.data(), z.size());
        CHECK(rz.has_value());
        if (rz) CHECK_EQ(get_string(*rz, "texte"), std::string("bonjour"));
        // parse_auto doit reconnaitre zlib tout seul
        auto ra = parse_auto(z.data(), z.size());
        CHECK(ra.has_value());

        const auto g = gzip_compress(b.v);
        CHECK(!g.empty());
        auto rg = parse_gzip(g.data(), g.size());
        CHECK(rg.has_value());
        if (rg) CHECK_EQ(get_num(*rg, "entier"), 123456789LL);
        auto rga = parse_auto(g.data(), g.size());
        CHECK(rga.has_value());

        // parse_auto sur du NBT brut
        auto rr = parse_auto(b.v.data(), b.v.size());
        CHECK(rr.has_value());
    }

    // =====================================================================
    // 4. Entrees invalides : nullopt, jamais de crash ni d'allocation folle
    // =====================================================================
    CHECK(!parse(nullptr, 0).has_value());
    CHECK(!parse_auto(nullptr, 0).has_value());
    {
        const std::uint8_t tiny[] = {10};
        CHECK(!parse(tiny, 1).has_value());
    }
    {
        // Racine qui n'est pas un compound
        const std::uint8_t notcomp[] = {3, 0, 0, 0, 0, 0, 1};
        CHECK(!parse(notcomp, sizeof(notcomp)).has_value());
    }
    {
        // Type de tag inconnu (99)
        Buf p; p.u8(10); p.str(""); p.u8(99);
        CHECK(!parse(p.v.data(), p.v.size()).has_value());
    }
    {
        // Longueur de tableau enorme annoncee, donnees absentes : doit etre
        // refuse AVANT toute allocation.
        Buf p; p.u8(10); p.str("");
        p.tag(11, "x"); p.i32(2000000000); // 2 milliards d'entiers
        CHECK(!parse(p.v.data(), p.v.size()).has_value());
    }
    {
        // Longueur negative
        Buf p; p.u8(10); p.str("");
        p.tag(7, "x"); p.i32(-1);
        CHECK(!parse(p.v.data(), p.v.size()).has_value());
    }
    {
        // Troncature au milieu d'une valeur
        Buf p; p.u8(10); p.str("");
        p.tag(4, "x"); p.u8(1); p.u8(2); // long incomplet
        CHECK(!parse(p.v.data(), p.v.size()).has_value());
    }
    {
        // Imbrication abusive : au-dela de kMaxDepth, refus sans debordement
        Buf p; p.u8(10); p.str("");
        for (int i = 0; i < kMaxDepth + 10; ++i) p.tag(10, "n");
        for (int i = 0; i < kMaxDepth + 10; ++i) p.u8(0);
        p.u8(0);
        CHECK(!parse(p.v.data(), p.v.size()).has_value());
    }
    {
        // zlib/gzip illisibles
        const std::uint8_t junk[] = {0x78, 0x9C, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK(!parse_zlib(junk, sizeof(junk)).has_value());
        const std::uint8_t gz[] = {0x1F, 0x8B, 0x08, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
        CHECK(!parse_gzip(gz, sizeof(gz)).has_value());
    }
    CHECK(!read_file("nexistepas.dat").has_value());

    // =====================================================================
    // 5. Fichier reel : level.dat d'une instance de l'utilisateur si presente
    // =====================================================================
    {
        const char* home = std::getenv("LOCALAPPDATA");
        bool tested = false;
        if (home) {
            const fs::path root = fs::path(home) / "TeamLauncher" / "instances";
            std::error_code ec;
            if (fs::is_directory(root, ec)) {
                for (fs::recursive_directory_iterator it(root, ec), end;
                     it != end && !tested; it.increment(ec)) {
                    if (ec) break;
                    if (it->path().filename() != "level.dat") continue;
                    auto lv = read_file(it->path().string());
                    CHECK(lv.has_value());
                    if (lv) {
                        // level.dat : racine vide contenant « Data »
                        const Compound* data = get_compound(*lv, "Data");
                        CHECK(data != nullptr);
                        if (data) {
                            const std::string name = get_string(*data, "LevelName");
                            std::printf("INFO level.dat lu : LevelName=\"%s\" "
                                        "version=%lld\n",
                                        name.c_str(), get_num(*data, "DataVersion"));
                            CHECK(!data->empty());
                        }
                    }
                    tested = true;
                }
            }
        }
        if (!tested)
            std::printf("INFO aucun level.dat trouve (test sur monde reel saute)\n");
    }

    // =====================================================================
    // 6. Ecriture : aller-retour complet (le C# ecrivait en petit-boutiste)
    // =====================================================================
    {
        Compound c;
        c["octet"] = make_byte(-3);
        c["court"] = make_short(-300);
        c["entier"] = make_int(123456);
        c["long"] = make_long(-9000000000LL);
        c["flottant"] = make_float(1.25f);
        c["double"] = make_double(-2.75);
        c["texte"] = make_string("accents éàü");
        c["octets"] = make_byte_array({1, 2, 255});
        c["entiers"] = make_int_array({-1, 0, 7});
        c["longs"] = make_long_array({-2, 99});
        Compound sub;
        sub["cle"] = make_string("valeur");
        c["sous"] = make_compound(sub);
        List items;
        Compound e1; e1["n"] = make_int(1); items.push_back(make_compound(e1));
        Compound e2; e2["n"] = make_int(2); items.push_back(make_compound(e2));
        c["liste"] = make_list(Type::Compound, items);
        c["vide"] = make_list(Type::Compound, {});

        const auto raw = write(c, "Racine");
        auto back = parse(raw.data(), raw.size());
        CHECK(back.has_value());
        if (back) {
            CHECK_EQ(get_num(*back, "octet"), -3LL);
            CHECK_EQ(get_num(*back, "court"), -300LL);
            CHECK_EQ(get_num(*back, "entier"), 123456LL);
            CHECK_EQ(get_num(*back, "long"), -9000000000LL);
            CHECK_EQ(get_dbl(*back, "flottant"), 1.25);
            CHECK_EQ(get_dbl(*back, "double"), -2.75);
            CHECK_EQ(get_string(*back, "texte"), std::string("accents éàü"));
            const Tag* oc = find(*back, "octets");
            CHECK(oc && oc->bytes.size() == 3 && oc->bytes[2] == 255);
            const Tag* en = find(*back, "entiers");
            CHECK(en && en->ints.size() == 3 && en->ints[0] == -1);
            const Tag* lo = find(*back, "longs");
            CHECK(lo && lo->longs.size() == 2 && lo->longs[0] == -2);
            const Compound* s2 = get_compound(*back, "sous");
            CHECK(s2 && get_string(*s2, "cle") == "valeur");
            const List* l2 = get_list(*back, "liste");
            CHECK(l2 && l2->size() == 2);
            const List* v2 = get_list(*back, "vide");
            CHECK(v2 && v2->empty());
        }
        // Le premier octet est TAG_Compound et le nom de racine suit, en
        // gros-boutiste : « 00 06 R a c i n e ».
        CHECK(raw.size() > 9);
        if (raw.size() > 9) {
            CHECK_EQ(raw[0], 10);
            CHECK_EQ(raw[1], 0);
            CHECK_EQ(raw[2], 6);
            CHECK_EQ(raw[3], static_cast<std::uint8_t>(0x52)); // R
        }
        // Aller-retour compresse
        const auto gz = write_gzip(c, "Racine");
        auto bg = parse_auto(gz.data(), gz.size());
        CHECK(bg.has_value());
        if (bg) CHECK_EQ(get_num(*bg, "entier"), 123456LL);
        const auto zl = write_zlib(c, "Racine");
        auto bz = parse_auto(zl.data(), zl.size());
        CHECK(bz.has_value());
        if (bz) CHECK_EQ(get_string(*bz, "texte"), std::string("accents éàü"));
    }

    if (g_failures == 0) {
        std::printf("ALL TESTS PASSED\n");
        return 0;
    }
    std::printf("%d TEST(S) FAILED\n", g_failures);
    return 1;
}

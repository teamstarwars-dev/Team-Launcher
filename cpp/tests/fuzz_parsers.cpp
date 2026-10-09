// Fuzzing à mutations des parseurs qui lisent des octets venus de
// l'extérieur.
//
// Ces parseurs sont la surface d'attaque la plus exposée du launcher :
// un monde Minecraft, un fichier de région ou un manifeste de plugin
// sont des données que l'utilisateur n'a pas écrites et que personne n'a
// validées. Un fichier conçu pour nuire n'a pas besoin d'être un monde
// valide — il lui suffit d'un champ de longueur mensonger.
//
// Pas de libFuzzer : le paquet n'est pas disponible sur cette machine.
// Ce harnais fait l'essentiel de ce qui compte — produire beaucoup
// d'entrées mal formées à partir de graines VALIDES, pour rester dans
// les chemins de code profonds plutôt que de se faire rejeter à la
// première vérification — et il tourne sous ASan + UBSan, qui sont ce
// qui transforme un dépassement silencieux en échec visible.
//
// Déterministe : la graine du générateur est fixe, donc un plantage se
// reproduit à l'identique. L'entrée fautive est écrite sur le disque.

#include "nbt.hpp"
#include "plugins.hpp"
#include "region.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace tl;

namespace {

std::mt19937 g_rng(0xC0FFEEu); // graine fixe : rejouable à l'identique
long long g_cases = 0;

std::size_t rnd(std::size_t n) {
    return n == 0 ? 0 : std::uniform_int_distribution<std::size_t>(0, n - 1)(g_rng);
}

// --- Graines valides ------------------------------------------------------
// Partir d'une entrée bien formée est ce qui distingue un fuzzing utile
// d'un générateur d'octets aléatoires : sans graine, tout est rejeté dès
// l'en-tête et on ne teste rien de profond.

std::vector<std::uint8_t> seed_nbt() {
    nbt::Compound root;

    nbt::Tag s;
    s.type = nbt::Type::String;
    s.str = "valeur";
    root["cle"] = s;

    nbt::Tag n;
    n.type = nbt::Type::Int;
    n.num = 42;
    root["nombre"] = n;

    nbt::Tag arr;
    arr.type = nbt::Type::ByteArray;
    arr.bytes.assign(64, 7);
    root["octets"] = arr;

    nbt::Tag ints;
    ints.type = nbt::Type::IntArray;
    ints.ints = {1, 2, 3, 4, 5};
    root["entiers"] = ints;

    nbt::Tag lst;
    lst.type = nbt::Type::List;
    lst.listElem = nbt::Type::Int;
    lst.list = std::make_shared<nbt::List>();
    for (int i = 0; i < 8; ++i) {
        nbt::Tag e;
        e.type = nbt::Type::Int;
        e.num = i;
        lst.list->push_back(e);
    }
    root["liste"] = lst;

    // Un niveau d'imbrication : les parseurs récursifs cassent plus
    // volontiers sous un sous-arbre que sur un tag plat.
    nbt::Tag sub;
    sub.type = nbt::Type::Compound;
    sub.comp = std::make_shared<nbt::Compound>();
    (*sub.comp)["dedans"] = s;
    root["sous"] = sub;

    return nbt::write(root, "racine");
}

// Fichier de région : 4 Kio de table de localisation, 4 Kio
// d'horodatages, puis les chunks. C'est la structure que list_chunks et
// read_chunk_local parcourent, en faisant confiance aux décalages.
std::vector<std::uint8_t> seed_region(const std::vector<std::uint8_t>& chunk) {
    const std::uint32_t sectors =
        static_cast<std::uint32_t>((chunk.size() + 5 + 4095) / 4096);
    std::vector<std::uint8_t> out(2 * 4096 + sectors * 4096, 0);
    out[0] = 0;
    out[1] = 0;
    out[2] = 2; // décalage : secteur 2
    out[3] = static_cast<std::uint8_t>(sectors);
    const std::uint32_t len = static_cast<std::uint32_t>(chunk.size() + 1);
    const std::size_t p = 2 * 4096;
    out[p + 0] = static_cast<std::uint8_t>(len >> 24);
    out[p + 1] = static_cast<std::uint8_t>(len >> 16);
    out[p + 2] = static_cast<std::uint8_t>(len >> 8);
    out[p + 3] = static_cast<std::uint8_t>(len);
    out[p + 4] = 2; // zlib
    if (!chunk.empty()) std::memcpy(out.data() + p + 5, chunk.data(), chunk.size());
    return out;
}

// --- Mutations ------------------------------------------------------------
// Les quatre premières sont génériques ; la cinquième vise précisément ce
// qui casse un parseur binaire — un champ de longueur qui ment.

void mutate(std::vector<std::uint8_t>& b) {
    if (b.empty()) return;
    switch (rnd(6)) {
        case 0:
            b[rnd(b.size())] ^= static_cast<std::uint8_t>(1u << rnd(8));
            break;
        case 1:
            b[rnd(b.size())] = static_cast<std::uint8_t>(rnd(256));
            break;
        case 2: // troncature : la moitié des plantages de parseurs
            b.resize(rnd(b.size()) + 1);
            break;
        case 3:
            b.insert(b.begin() + static_cast<long>(rnd(b.size())),
                     static_cast<std::uint8_t>(rnd(256)));
            break;
        case 4: { // longueur énorme, négative, ou nulle
            if (b.size() < 4) break;
            const std::size_t p = rnd(b.size() - 3);
            static const std::uint32_t evil[] = {0xFFFFFFFFu, 0x7FFFFFFFu,
                                                 0x80000000u, 0x00FFFFFFu, 0u};
            const std::uint32_t v = evil[rnd(5)];
            b[p + 0] = static_cast<std::uint8_t>(v >> 24);
            b[p + 1] = static_cast<std::uint8_t>(v >> 16);
            b[p + 2] = static_cast<std::uint8_t>(v >> 8);
            b[p + 3] = static_cast<std::uint8_t>(v);
            break;
        }
        default: {
            const std::size_t n = 1 + rnd(16);
            for (std::size_t i = 0; i < n && !b.empty(); ++i)
                b[rnd(b.size())] = static_cast<std::uint8_t>(rnd(256));
            break;
        }
    }
}

void dump(const char* what, long long iter, const std::string& data) {
    const fs::path p =
        fs::temp_directory_path() /
        (std::string("tl_fuzz_") + what + "_" + std::to_string(iter) + ".bin");
    std::ofstream o(p, std::ios::binary);
    o.write(data.data(), static_cast<std::streamsize>(data.size()));
    std::printf("  entrée fautive écrite : %s\n", p.string().c_str());
}

// --- Cibles ---------------------------------------------------------------

void fuzz_nbt(int iterations) {
    const auto base = seed_nbt();
    if (base.empty()) {
        std::printf("\nECHEC : la graine NBT est vide (nbt::write a échoué)\n");
        std::exit(1);
    }
    for (int i = 0; i < iterations; ++i) {
        auto b = base;
        const int rounds = 1 + static_cast<int>(rnd(4));
        for (int r = 0; r < rounds; ++r) mutate(b);
        g_cases++;
        // Le contrat est « rend nullopt sur entrée invalide », jamais
        // « plante » ni « lit hors limites ». ASan le vérifie pour nous.
        (void)nbt::parse(b.data(), b.size());
        (void)nbt::parse_auto(b.data(), b.size());
        (void)nbt::parse_zlib(b.data(), b.size());
        (void)nbt::parse_gzip(b.data(), b.size());
        if ((i % 2000) == 0) { std::printf("."); std::fflush(stdout); }
    }
}

void fuzz_region(int iterations) {
    nbt::Compound c;
    nbt::Tag t;
    t.type = nbt::Type::Int;
    t.num = 1;
    c["DataVersion"] = t;
    const auto base = seed_region(nbt::write_zlib(c, ""));

    std::error_code ec;
    const fs::path tmp = fs::temp_directory_path() / "tl_fuzz_region";
    fs::create_directories(tmp, ec);
    const fs::path rf = tmp / "r.0.0.mca";

    for (int i = 0; i < iterations; ++i) {
        auto b = base;
        const int rounds = 1 + static_cast<int>(rnd(5));
        for (int r = 0; r < rounds; ++r) mutate(b);
        {
            std::ofstream o(rf, std::ios::binary | std::ios::trunc);
            o.write(reinterpret_cast<const char*>(b.data()),
                    static_cast<std::streamsize>(b.size()));
        }
        g_cases++;
        (void)region::list_chunks(rf);
        (void)region::read_chunk_local(rf, 0, 0);
        if ((i % 500) == 0) { std::printf("."); std::fflush(stdout); }
    }
    fs::remove_all(tmp, ec);
}

// --- Décodage d'un chunk ---------------------------------------------
//
// Trou comblé : la campagne précédente n'appelait que list_chunks et
// read_chunk_local. Elle ne touchait donc JAMAIS ChunkView::at, ni
// block_at, heightmap ou unique_blocks — c'est-à-dire le dépaquetage de
// bits, le code le plus retors du module. Un fichier de région mutilé
// échoue presque toujours à la décompression zlib, si bien que le
// décodeur de sections n'était quasiment jamais atteint.
//
// On mute donc le NBT du chunk DIRECTEMENT, en partant d'un chunk
// réaliste, pour entrer dans le décodeur avec des palettes et des
// tableaux de bits incohérents.

nbt::Compound seed_chunk(bool legacy) {
    nbt::Compound root;
    nbt::Tag dv;
    dv.type = nbt::Type::Int;
    dv.num = legacy ? 1343 : 3700; // 1.12 vs 1.20+
    root["DataVersion"] = dv;

    nbt::Tag sec;
    sec.type = nbt::Type::Compound;
    sec.comp = std::make_shared<nbt::Compound>();

    nbt::Tag y;
    y.type = nbt::Type::Byte;
    y.num = 0;
    (*sec.comp)["Y"] = y;

    if (legacy) {
        // Format 1.12 : identifiants bruts, plus le demi-octet « Add ».
        nbt::Tag blocks;
        blocks.type = nbt::Type::ByteArray;
        blocks.bytes.assign(4096, 1);
        (*sec.comp)["Blocks"] = blocks;
        nbt::Tag add;
        add.type = nbt::Type::ByteArray;
        add.bytes.assign(2048, 0x12);
        (*sec.comp)["Add"] = add;
    } else {
        // Format moderne : palette + indices empaquetés.
        nbt::Tag bs;
        bs.type = nbt::Type::Compound;
        bs.comp = std::make_shared<nbt::Compound>();

        nbt::Tag pal;
        pal.type = nbt::Type::List;
        pal.listElem = nbt::Type::Compound;
        pal.list = std::make_shared<nbt::List>();
        for (const char* n : {"minecraft:air", "minecraft:stone",
                              "minecraft:dirt", "minecraft:grass_block"}) {
            nbt::Tag e;
            e.type = nbt::Type::Compound;
            e.comp = std::make_shared<nbt::Compound>();
            nbt::Tag nm;
            nm.type = nbt::Type::String;
            nm.str = n;
            (*e.comp)["Name"] = nm;
            pal.list->push_back(e);
        }
        (*bs.comp)["palette"] = pal;

        // 4 entrées -> 2 bits chacune, mais Minecraft impose un minimum
        // de 4 bits : 16 indices par long, donc 256 longs pour 4096.
        nbt::Tag data;
        data.type = nbt::Type::LongArray;
        data.longs.assign(256, 0x0123456789ABCDEFll);
        (*bs.comp)["data"] = data;

        (*sec.comp)["block_states"] = bs;
    }

    nbt::Tag secs;
    secs.type = nbt::Type::List;
    secs.listElem = nbt::Type::Compound;
    secs.list = std::make_shared<nbt::List>();
    secs.list->push_back(sec);
    root[legacy ? "Sections" : "sections"] = secs;

    if (legacy) {
        // 1.12 range tout sous « Level ».
        nbt::Compound outer;
        nbt::Tag lvl;
        lvl.type = nbt::Type::Compound;
        lvl.comp = std::make_shared<nbt::Compound>(root);
        outer["Level"] = lvl;
        return outer;
    }
    return root;
}

void fuzz_chunk(int iterations) {
    const std::vector<std::uint8_t> bases[] = {nbt::write(seed_chunk(false), ""),
                                               nbt::write(seed_chunk(true), "")};
    // Altitudes volontairement hors monde aux extrémités : c'est là que
    // se cache un index négatif ou un débordement de section.
    static const int ys[] = {-2048, -64, -1, 0, 1, 15, 16, 127, 319, 320, 4096};
    static const int xs[] = {0, 7, 15};

    // Compteurs de COUVERTURE : sans eux, une cible qui ne plante jamais
    // peut aussi bien ne rien tester. C'est précisément ce qui était
    // arrivé à la cible « Région » pour le décodeur de sections.
    long long parsed = 0, decoded = 0;
    for (int i = 0; i < iterations; ++i) {
        auto b = bases[i & 1];
        const int rounds = 1 + static_cast<int>(rnd(4));
        for (int r = 0; r < rounds; ++r) mutate(b);
        g_cases++;
        const auto c = nbt::parse(b.data(), b.size());
        if (!c) continue; // mutation trop destructrice : rien à décoder
        ++parsed;

        region::ChunkView v(*c);
        bool hit = false;
        for (const int yy : ys)
            for (const int xx : xs) {
                if (!v.at(xx, yy, xx).empty()) hit = true;
                (void)region::block_at(*c, xx, yy, xx);
            }
        if (hit) ++decoded;
        (void)region::heightmap(*c);
        (void)region::unique_blocks(*c);
        if ((i % 1000) == 0) { std::printf("."); std::fflush(stdout); }
    }
    std::printf(" [%lld analysés, %lld avec un bloc décodé]", parsed, decoded);
    // Garde-fou : si plus rien n'atteint le décodeur, la cible est creuse
    // et doit échouer bruyamment plutôt que de rassurer à tort.
    if (iterations >= 1000 && decoded == 0) {
        std::printf("\nECHEC : aucun bloc décodé — la cible n'atteint pas "
                    "le décodeur de sections\n");
        std::exit(1);
    }
}

// Le manifeste de plugin est du texte, mais il vient d'un dossier que
// n'importe qui peut déposer.
void fuzz_manifest(int iterations) {
    static const char* bases[] = {
        R"J({"name":"Test","version":"1.0","actions":[{"label":"A","run":"prog","args":["{instanceDir}"]}]})J",
        R"J({"name":"X","actions":[]})J",
        R"J({"name":"Y","actions":[{"label":"L","run":"p","args":["-a","-b"],"where":"instance"}]})J",
        R"J({"actions":[{"label":"Z","run":"p","workDir":"/tmp"}]})J",
    };
    for (int i = 0; i < iterations; ++i) {
        std::string s = bases[rnd(4)];
        std::vector<std::uint8_t> b(s.begin(), s.end());
        const int rounds = 1 + static_cast<int>(rnd(3));
        for (int r = 0; r < rounds; ++r) mutate(b);
        s.assign(b.begin(), b.end());
        g_cases++;
        const auto p = plugins::parse_manifest(s, "fuzz");

        // Invariant de sécurité. Le module ne promet AUCUN bac à sable,
        // mais il promet ceci : un « run » retenu est un programme, pas
        // une ligne de shell. C'est sur quoi repose le fait de lancer
        // sans shell. Les ARGUMENTS, eux, peuvent contenir n'importe
        // quoi — ils sont passés séparément au système et un « && » y
        // reste du texte ; les vérifier serait signaler un faux défaut.
        for (const auto& a : p.actions)
            if (a.program.find_first_of("&|<>;\n") != std::string::npos) {
                std::printf(
                    "\nECHEC itération %d : séparateur accepté dans run « %s »\n",
                    i, a.program.c_str());
                dump("manifest", i, s);
                std::exit(1);
            }
        if ((i % 2000) == 0) { std::printf("."); std::fflush(stdout); }
    }
}

} // namespace

int main(int argc, char** argv) {
    // Court par défaut, pour rester utilisable dans une passe de tests.
    // « fuzz_parsers 50000 » pour une campagne plus longue.
    int n = 4000;
    if (argc > 1) n = std::atoi(argv[1]);
    if (n < 1) n = 1;

    std::printf("Fuzzing des parseurs (%d itérations par cible)\n", n);
    std::printf("NBT       "); fuzz_nbt(n);            std::printf(" ok\n");
    std::printf("Région    "); fuzz_region(n / 8 + 1); std::printf(" ok\n");
    std::printf("Chunk     "); fuzz_chunk(n);           std::printf(" ok\n");
    std::printf("Manifeste "); fuzz_manifest(n);       std::printf(" ok\n");
    std::printf("\n%lld cas traités, aucun plantage.\n", g_cases);
    std::printf("ALL TESTS PASSED\n");
    return 0;
}

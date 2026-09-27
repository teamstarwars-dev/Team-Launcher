#pragma once

// ---------------------------------------------------------------------------
// obf.hpp — barriere « lecture de chaines » (Securite S3, header-only).
//
// Objectif : les chaines d'infrastructure (webhook Discord, IP d'admin,
// endpoints d'authentification) n'apparaissent PAS en clair dans le binaire :
// une simple commande `strings` sur l'executable ne doit remonter que des
// octets illisibles.
//
// CE N'EST PAS DU CHIFFREMENT CRYPTOGRAPHIQUE. Le dechiffrement est un XOR
// avec une cle derivee a la compilation : tout adversaire disposant du binaire
// peut reconstituer la valeur en lisant le code de dechiffrement. Il s'agit
// uniquement d'une barriere contre la lecture passive (dump `strings`, copie
// du .rdata). Les vrais secrets (jetons, cles API) restent du cote de
// secrets.hpp (DPAPI), pas ici.
//
// Mecanisme :
//   - encode() est `consteval` : le literal en clair n'existe qu'a la
//     compilation, il n'est JAMAIS transmis a une fonction d'execution ni
//     stocke dans .rdata ;
//   - seul le tableau XORe (Blob) est emis en .rdata, cle comprise, mais le
//     clair n'y figure jamais de facon contigue ;
//   - decode() ne tourne qu'a l'execution, dans un buffer local retourne par
//     valeur ; les lectures `volatile` empechent l'optimiseur (et LTO) de
//     replier la boucle et de re-materialiser le clair en literal.
//
// Usages :
//   static const std::string kUrl = TL_OBF("https://..."); // a l'init
//   const std::string url = TL_OBF("https://...");         // a la volee
//
// Type-safe : le macro n'accepte qu'un literal de caracteres (deduction de
// `char[N]` sur encode) ; passer une variable `const char*` ne compile pas.
// ---------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace tl::obf {

// Sel fixe du projet. Ce n'est pas un secret : il evite seulement des cles
// triviales (tout a zero), la diversite vient de __LINE__ / __COUNTER__.
inline constexpr std::uint32_t kSalt = 0x544C4F42u; // « TLOB » en ASCII

// Octet de cle a la position i pour un site d'appel (ligne, compteur).
// Hachage court type murmur (avalanche) : deux sites voisins ont des cles
// disjoints. `constexpr` (et non consteval) car decode() doit y acceder aussi
// a l'execution, avec exactement les memes arguments.
// Jamais nul : un octet de cle nul laisserait l'octet de clair tel quel.
constexpr std::uint8_t key_byte(std::size_t line, std::size_t site,
                                std::size_t i) {
    std::uint32_t h = kSalt;
    h ^= static_cast<std::uint32_t>(line + 1) * 0x9E3779B1u;
    h ^= static_cast<std::uint32_t>(site + 1) * 0x85EBCA6Bu;
    h ^= static_cast<std::uint32_t>(i + 1) * 0xC2B2AE35u;
    h ^= h >> 16;
    h *= 0x7FEB352Du;
    h ^= h >> 15;
    h *= 0x846CA68Bu;
    h ^= h >> 16;
    const std::uint8_t k = static_cast<std::uint8_t>(h);
    return k == 0 ? static_cast<std::uint8_t>(0xA5) : k;
}

// Representation chiffree d'une chaine : octets XOR cle, sans le '\0' final.
// N, Line et Site font partie du type => la cle est liee a la representation
// (decode() derive toujours la bonne cle a partir du type recu).
template <std::size_t N, std::size_t Line, std::size_t Site>
struct Blob {
    std::array<char, N> x{};
};

// Encodage PUREMENT compile-time (consteval) : garantit que le literal en
// clair ne peut jamais fuiter dans le code machine genere. La chaine entree
// est un `char[N]` : seuls les literals (et les concat adjacentes de literals)
// sont acceptes.
template <std::size_t Line, std::size_t Site, std::size_t N>
consteval Blob<N - 1, Line, Site> encode(const char (&s)[N]) {
    Blob<N - 1, Line, Site> b{};
    for (std::size_t i = 0; i < N - 1; ++i)
        b.x[i] = static_cast<char>(static_cast<unsigned char>(s[i]) ^
                                   key_byte(Line, Site, i));
    return b;
}

// Dechiffrement a l'execution, buffer local (std::string par valeur) : le
// clair n'existe que le temps de l'appel, jamais dans .rdata.
// Les lectures `volatile` sont des barrieres de non-propagation de constantes
// : l'optimiseur ne peut pas pre-calculer la boucle, donc il ne peut pas
// re-materialiser la chaine en literal sous LTO /OPT:ICF.
template <std::size_t N, std::size_t Line, std::size_t Site>
std::string decode(const Blob<N, Line, Site>& b) {
    std::string out(N, '\0');
    for (std::size_t i = 0; i < N; ++i) {
        const volatile unsigned char e = static_cast<unsigned char>(b.x[i]);
        const volatile unsigned char k = key_byte(Line, Site, i);
        out[i] = static_cast<char>(static_cast<unsigned char>(e ^ k));
    }
    return out;
}

} // namespace tl::obf

// Type-safe : n'accepte qu'un literal ("..."), jamais une variable.
// __LINE__ + __COUNTER__ donnent une cle unique a chaque site d'appel.
// Remarque : dans un header inclu par plusieurs TUs, __COUNTER__ differe
// d'une TU a l'autre ; chaque expansion reste autonome (cle + decodage colles
// l'un a l'autre, meme taille de champ) donc le resultat est identique.
#define TL_OBF(lit)                                                       \
    (::tl::obf::decode(::tl::obf::encode<__LINE__, __COUNTER__>(lit)))

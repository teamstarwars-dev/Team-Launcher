#pragma once

// Phase 5 — mises a jour des mods, avec leur changelog.
//
// Le chemin est celui que Modrinth prevoit pour cela, et il ne demande pas
// de savoir d'ou vient le fichier : on calcule le SHA-1 de chaque .jar,
// l'API rend le projet auquel il appartient, puis on demande les versions
// de ce projet compatibles avec le chargeur et la version de Minecraft de
// l'instance. Un mod telecharge a la main, hors du launcher, est donc
// reconnu comme les autres.
//
// Le changelog compte autant que la mise a jour elle-meme : mettre a jour
// un mod a l'aveugle, en cours de partie, casse des mondes. On le rapporte
// pour que la decision soit prise en connaissance de cause.
//
// Ce que ce module NE fait pas : CurseForge. Son API exige une cle, que
// tout le monde n'a pas, et son point d'entree par empreinte est un autre
// protocole (murmur2). Les mods qui n'existent que la ressortent en
// « inconnu », pas en « a jour » — on ne laisse pas croire a une
// verification qui n'a pas eu lieu.

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tl::modupdate {

struct Update {
    std::filesystem::path file;  // .jar installe
    std::string displayName;     // nom lisible (manifeste, sinon fichier)
    std::string projectId;
    std::string currentVersion;  // vide = non declaree
    std::string newVersion;
    std::string changelog;       // texte de l'auteur, souvent en Markdown
    std::string downloadUrl;
    std::string newFilename;
};

struct Result {
    std::vector<Update> updates;
    int checked = 0;   // mods soumis a la verification
    int unknown = 0;   // introuvables sur Modrinth
    int upToDate = 0;
    std::string error; // non vide = la verification n'a pas pu aboutir
};

using Progress = std::function<void(int done, int total)>;

// Examine les .jar ACTIFS du dossier (les desactives ne seront pas
// charges, les mettre a jour n'aurait pas de sens). Ne leve pas.
Result check(const std::filesystem::path& modsDir, const std::string& loader,
             const std::string& mcVersion,
             const std::atomic<bool>* cancel = nullptr,
             Progress progress = {});

// Telecharge la nouvelle version a cote, puis retire l'ancienne SEULEMENT
// si l'ecriture a reussi : une coupure reseau ne doit pas laisser
// l'instance sans le mod. false + errOut sinon.
bool apply(const Update& u, const std::atomic<bool>* cancel = nullptr,
           std::string* errOut = nullptr);

} // namespace tl::modupdate

#pragma once

// Les deux analyses, vues comme « un corps de requête entre, un corps de
// réponse sort ». Aucune notion de HTTP, de clé ni de réseau ici.
//
// Pourquoi ce module existe : les mêmes analyses sont servies par DEUX
// programmes — le launcher (`localapi`) et le service autonome
// (`tl_diagd`). Dupliquer les gestionnaires aurait garanti qu'ils
// divergent, et c'est exactement le genre de divergence qu'on ne remarque
// que le jour où un appelant reçoit deux réponses différentes pour la
// même question.
//
// Conséquence utile : tout est testable sans lancer de serveur, et le
// service hébergé n'embarque ni SDL, ni SDK Discord, ni configuration de
// launcher — rien que `std`, nlohmann et miniz.

#include <string>

namespace tl::diagapi {

struct Result {
    int status = 200;
    std::string body; // toujours du JSON, même en erreur
};

// Corps = log brut, ou {"log":"..."}. Les deux sont acceptés : imposer un
// échappement JSON sur 400 Ko de texte serait une corvée gratuite.
Result crash(const std::string& body);

// Corps = {"loader":"...", "mcVersion":"...", "mods":[...]}.
Result mods(const std::string& body);

// Auto-description de l'API.
std::string describe();

} // namespace tl::diagapi

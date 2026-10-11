#pragma once
// Processus enfant relié par DEUX tubes anonymes (son entrée et sa sortie
// standard) — V8, plugins.
//
// Sert aux deux liens du système de plugins : le launcher lance le plugin
// host, et le host lance chaque plugin.
//
// Pourquoi des tubes anonymes plutôt que des tubes nommés ou des sockets :
// un tube nommé est joignable par n'importe quel processus de la session,
// et un programme malveillant peut le créer AVANT nous pour intercepter la
// connexion. Un tube anonyme n'existe que pour le processus qui l'hérite.
// C'est le modèle de l'extension host de VS Code.
//
// Ce que l'enfant hérite, et seulement cela :
//   - Windows : la liste des handles hérités est RESTREINTE explicitement
//     (PROC_THREAD_ATTRIBUTE_HANDLE_LIST) aux trois handles de ses flux. Par
//     défaut, un enfant hérite de TOUT handle héritable du parent.
//   - POSIX : tous les descripteurs au-delà de 2 sont fermés chez l'enfant
//     (closefrom), en plus du O_CLOEXEC de nos propres tubes.
//
// Durée de vie :
//   - Windows : l'enfant est placé dans un objet job « tuer à la
//     fermeture ». Si le parent meurt, même brutalement, ses enfants
//     meurent avec lui — un plugin ne survit pas à son host.
//   - POSIX : l'enfant a son propre groupe de processus, que terminate()
//     tue en entier (le plugin ET ce qu'il aurait lancé).
//
// Module autonome (bibliothèque standard + API système), lié aussi par
// tl_plugin_host, qui ne lie pas tl_core.

#include <cstddef>
#include <string>
#include <vector>

namespace tl::pipechild {

struct Options {
    std::string program;            // chemin ou nom (cherché dans le PATH)
    std::vector<std::string> args;  // SANS argv[0]
    std::string workDir;            // vide = hérité
    std::string stderrPath;         // vide = vers le néant
};

class Child {
public:
    Child() = default;
    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    ~Child(); // termine l'enfant s'il tourne encore

    // Lance le processus. Aucun shell : programme et arguments sont passés
    // séparément au système.
    bool start(const Options& o, std::string* err);

    // Écrit tout, ou rend false (enfant mort, tube fermé).
    bool write_all(const char* data, std::size_t n);

    // Lecture bloquante : > 0 octets lus, 0 fin de flux, < 0 erreur.
    long read_some(char* buf, std::size_t cap);

    void close_stdin();
    void terminate();
    bool alive();
    long pid() const;

private:
#ifdef _WIN32
    void* proc_ = nullptr;
    void* job_ = nullptr;
    void* inWr_ = nullptr;  // vers l'entrée de l'enfant
    void* outRd_ = nullptr; // depuis la sortie de l'enfant
    unsigned long pid_ = 0;
#else
    int pid_ = -1;
    int inWr_ = -1;
    int outRd_ = -1;
    bool reaped_ = false;
#endif
};

// Les flux standard du processus COURANT, vus comme un lien : c'est par là
// que le plugin host parle au launcher.
long stdio_read(char* buf, std::size_t cap);
bool stdio_write(const char* data, std::size_t n);

// Échappement d'un argument selon les règles de CommandLineToArgvW. Exposé
// pour les tests ; sans effet hors Windows, mais compilé partout.
std::string win_quote(const std::string& a);

} // namespace tl::pipechild

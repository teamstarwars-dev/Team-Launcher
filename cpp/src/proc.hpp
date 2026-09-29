#pragma once

// Lancement de processus POSIX (posix_spawn + pipes) pour game_launcher
// (jeu, detection Java) et moddev (gradle/outils).
//
// Sous Windows ce header est vide : les TU gardent leur code CreateProcess
// eprouve. Ici, jamais de fork() (dangereux en multithread) : posix_spawn
// + posix_spawn_file_actions_addchdir_np (glibc >= 2.29) pour le dossier de
// travail, repli sh -c ailleurs.
//
// Convention : argv[0] = chemin de l'executable (posix_spawnp cherche dans
// PATH si argv[0] ne contient pas de '/'). stdin de l'enfant = /dev/null
// (comme hStdInput = nullptr cote Windows).

#include <optional>
#include <string>
#include <vector>

#ifdef _WIN32
namespace tl::proc {
// Rien : CreateProcess cote TU.
} // namespace tl::proc
#else

#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace tl::proc {

// Enfant lance : fds lecture cote parent (-1 si fusionnes).
struct Child {
    pid_t pid = -1;
    int outFd = -1; // stdout (et stderr si mergeErr)
    int errFd = -1; // stderr, -1 si mergeErr
};

// Lance exe + argv (argv[0] inclus). workDir vide = herite. mergeErr fusionne
// stderr sur stdout. errOut (optionnel) recoit le code posix_spawn/errno.
inline std::optional<Child> spawn(const std::string& exe,
                                  const std::vector<std::string>& argv,
                                  const std::string& workDir, bool mergeErr,
                                  int* errOut = nullptr) {
    auto fail = [&](int e) -> std::optional<Child> {
        if (errOut) *errOut = e;
        return std::nullopt;
    };
    if (argv.empty()) return fail(EINVAL);

    // O_CLOEXEC : un spawn concurrent n'herite pas de nos tubes (sinon l'EOF
    // attendrait aussi l'autre enfant). Les dup2 cote enfant retirent le flag.
    int outPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    if (::pipe2(outPipe, O_CLOEXEC) != 0) return fail(errno);
    if (!mergeErr && ::pipe2(errPipe, O_CLOEXEC) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        return fail(errno);
    }

    struct FaGuard {
        posix_spawn_file_actions_t fa{};
        bool ok = false;
        FaGuard() { ok = ::posix_spawn_file_actions_init(&fa) == 0; }
        ~FaGuard() {
            if (ok) ::posix_spawn_file_actions_destroy(&fa);
        }
    } acts;
    if (!acts.ok) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        if (!mergeErr) {
            ::close(errPipe[0]);
            ::close(errPipe[1]);
        }
        return fail(ENOMEM);
    }
    auto& fa = acts.fa;
    ::posix_spawn_file_actions_addclose(&fa, outPipe[0]);
    if (!mergeErr) ::posix_spawn_file_actions_addclose(&fa, errPipe[0]);
    ::posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDOUT_FILENO);
    if (mergeErr)
        ::posix_spawn_file_actions_adddup2(&fa, outPipe[1], STDERR_FILENO);
    else
        ::posix_spawn_file_actions_adddup2(&fa, errPipe[1], STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&fa, outPipe[1]);
    if (!mergeErr) ::posix_spawn_file_actions_addclose(&fa, errPipe[1]);
    // stdin = /dev/null, comme hStdInput = nullptr cote Windows.
    if (::posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY,
                                           0) != 0) {
        ::close(outPipe[0]);
        ::close(outPipe[1]);
        if (!mergeErr) {
            ::close(errPipe[0]);
            ::close(errPipe[1]);
        }
        return fail(ENOENT);
    }

    std::vector<char*> av;
    av.reserve(argv.size() + 8);
    for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
    av.push_back(nullptr);

    pid_t pid = -1;
    int rc = 0;
    if (!workDir.empty()) {
#ifdef __GLIBC__
        rc = ::posix_spawn_file_actions_addchdir_np(&fa, workDir.c_str());
        if (rc == 0)
            rc = ::posix_spawnp(&pid, exe.c_str(), &fa, nullptr, av.data(), environ);
#else
        // Repli sans addchdir_np : sh -c 'cd "$0" && shift && exec "$@"'.
        // "$0" est quoté par construction (pas d'interpolation) : sûr.
        std::vector<std::string> shArgs = {"/bin/sh", "-c",
                                           "cd \"$0\" && shift && exec \"$@\"", workDir};
        shArgs.insert(shArgs.end(), argv.begin(), argv.end());
        std::vector<char*> shAv;
        for (auto& a : shArgs) shAv.push_back(a.data());
        shAv.push_back(nullptr);
        rc = ::posix_spawnp(&pid, "/bin/sh", &fa, nullptr, shAv.data(), environ);
#endif
    } else {
        rc = ::posix_spawnp(&pid, exe.c_str(), &fa, nullptr, av.data(), environ);
    }

    ::close(outPipe[1]);
    if (!mergeErr) ::close(errPipe[1]);
    if (rc != 0) {
        ::close(outPipe[0]);
        if (!mergeErr) ::close(errPipe[0]);
        return fail(rc);
    }
    if (errOut) *errOut = 0;
    return Child{pid, outPipe[0], mergeErr ? -1 : errPipe[0]};
}

// Variante shell (commande brute, comme la ligne CreateProcess) : stdout et
// stderr fusionnes, dossier de travail applique.
inline std::optional<Child> spawn_shell(const std::string& commandLine,
                                        const std::string& workDir,
                                        int* errOut = nullptr) {
    if (commandLine.empty()) {
        if (errOut) *errOut = EINVAL;
        return std::nullopt;
    }
    return spawn("/bin/sh", {"/bin/sh", "-c", commandLine}, workDir, true, errOut);
}

inline void close_fd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

// Ouvre une URL/document avec l'application associée (xdg-open détaché).
// Fire-and-forget : jamais bloquant, échec silencieux (comme ShellExecuteW).
inline void open_detached(const std::string& target) {
    if (target.empty()) return;
    auto c = spawn("xdg-open", {"xdg-open", target}, "", true);
    // Sortie minuscule : fermer le bout lecture de suite est sans risque.
    if (c) close_fd(c->outFd);
}

// SIGKILL (comme TerminateProcess). ESRCH = deja mort : succes.
inline bool terminate_child(pid_t pid) {
    return ::kill(pid, SIGKILL) == 0 || errno == ESRCH;
}

// Attend la fin. timeoutMs < 0 = infini. Retourne le code de sortie, -1 si
// signal/timeout/erreur (timeout : tue puis moissonne, pas de zombie).
inline int wait_exit(pid_t pid, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs < 0 ? 86400000 : timeoutMs);
    for (;;) {
        int st = 0;
        const pid_t r = ::waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(st)) return WEXITSTATUS(st);
            return -1;
        }
        if (r < 0 && errno != EINTR) return -1;
        if (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline) {
            terminate_child(pid);
            int st2 = 0;
            while (::waitpid(pid, &st2, 0) < 0 && errno == EINTR) {
            }
            return -1;
        }
        struct timespec ts{0, 50000000}; // 50 ms
        ::nanosleep(&ts, nullptr);
    }
}

// Lancement bref + capture fusionnee stdout/stderr, avec delai.
// code == -1 : lancement impossible ou delai depasse (tue + moissonne).
struct RunResult {
    int code = -1;
    std::string output;
};

inline RunResult run_capture(const std::string& exe, const std::vector<std::string>& argv,
                             const std::string& workDir, int timeoutMs) {
    RunResult r;
    auto c = spawn(exe, argv, workDir, true);
    if (!c) return r;
    // Draine pendant que l'enfant tourne (un enfant bavard > 64 Kio
    // bloquerait sinon sur un tube plein).
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs < 0 ? 86400000 : timeoutMs);
    char buf[4096];
    bool dead = false;
    for (;;) {
        int waitMs = -1;
        if (timeoutMs >= 0) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() < 0) {
                dead = true;
                break;
            }
            waitMs = static_cast<int>(left.count());
        }
        pollfd p{};
        p.fd = c->outFd;
        p.events = POLLIN;
        const int pr = ::poll(&p, 1, waitMs);
        if (pr <= 0) {
            dead = true;
            break; // delai (0) ou erreur (-1) : meme traitement
        }
        const ssize_t n = ::read(c->outFd, buf, sizeof(buf));
        if (n <= 0) break; // EOF ou lien coupe : l'enfant a fini (ou presque)
        r.output.append(buf, static_cast<size_t>(n));
    }
    int leftMs = -1;
    if (timeoutMs >= 0) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        leftMs = left.count() < 0 ? 0 : static_cast<int>(left.count());
    }
    if (dead) {
        terminate_child(c->pid);
        int st = 0;
        while (::waitpid(c->pid, &st, 0) < 0 && errno == EINTR) {
        }
        r.code = -1;
    } else {
        r.code = wait_exit(c->pid, leftMs);
    }
    close_fd(c->outFd);
    return r;
}

} // namespace tl::proc

#endif // _WIN32 / POSIX

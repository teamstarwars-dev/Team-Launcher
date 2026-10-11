#include "pipe_child.hpp"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <chrono>
#include <thread>

namespace tl::pipechild {

std::string win_quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) return a;
    std::string out = "\"";
    for (std::size_t i = 0;; ++i) {
        std::size_t slashes = 0;
        while (i < a.size() && a[i] == '\\') {
            ++i;
            ++slashes;
        }
        if (i == a.size()) {
            out.append(slashes * 2, '\\');
            break;
        }
        if (a[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
            out.push_back('"');
        } else {
            out.append(slashes, '\\');
            out.push_back(a[i]);
        }
    }
    out.push_back('"');
    return out;
}

#ifdef _WIN32

namespace {

std::wstring to_w(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    w.resize(static_cast<std::size_t>(n - 1));
    return w;
}

void close_h(void*& h) {
    if (h && h != INVALID_HANDLE_VALUE) CloseHandle(static_cast<HANDLE>(h));
    h = nullptr;
}

std::string last_error(const char* what) {
    return std::string(what) + " (code " + std::to_string(GetLastError()) + ")";
}

} // namespace

Child::~Child() {
    terminate();
    close_h(inWr_);
    close_h(outRd_);
    close_h(proc_);
    close_h(job_); // KILL_ON_JOB_CLOSE : dernier filet
}

bool Child::start(const Options& o, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (o.program.empty()) return fail("programme vide");

    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE inRd = nullptr, inWr = nullptr, outRd = nullptr, outWr = nullptr;
    if (!CreatePipe(&inRd, &inWr, &sa, 0)) return fail(last_error("CreatePipe"));
    if (!CreatePipe(&outRd, &outWr, &sa, 0)) {
        CloseHandle(inRd);
        CloseHandle(inWr);
        return fail(last_error("CreatePipe"));
    }
    // Nos extrémités ne doivent PAS être héritées.
    SetHandleInformation(inWr, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRd, HANDLE_FLAG_INHERIT, 0);

    HANDLE errH = INVALID_HANDLE_VALUE;
    const std::wstring errPath = o.stderrPath.empty() ? L"NUL" : to_w(o.stderrPath);
    errH = CreateFileW(errPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (errH == INVALID_HANDLE_VALUE)
        errH = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0,
                           nullptr);

    auto cleanup_child_side = [&]() {
        CloseHandle(inRd);
        CloseHandle(outWr);
        if (errH != INVALID_HANDLE_VALUE) CloseHandle(errH);
    };

    // Liste EXPLICITE des handles hérités : uniquement les trois flux.
    HANDLE inherit[3] = {inRd, outWr, errH};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuf(attrSize);
    auto* attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuf.data());
    if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) ||
        !UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                   sizeof(inherit), nullptr, nullptr)) {
        cleanup_child_side();
        CloseHandle(inWr);
        CloseHandle(outRd);
        return fail(last_error("liste de handles"));
    }

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = inRd;
    si.StartupInfo.hStdOutput = outWr;
    si.StartupInfo.hStdError = errH;
    si.lpAttributeList = attrs;

    std::string cmd = win_quote(o.program);
    for (const auto& a : o.args) cmd += " " + win_quote(a);
    std::wstring wcmd = to_w(cmd);
    wcmd.push_back(L'\0');
    const std::wstring wwork = to_w(o.workDir);

    PROCESS_INFORMATION pi{};
    // SUSPENDU : le placer dans le job AVANT qu'il exécute la moindre
    // instruction, pour qu'il ne puisse pas s'en échapper en lançant un
    // enfant dans la fenêtre.
    const BOOL ok = CreateProcessW(
        nullptr, wcmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        wwork.empty() ? nullptr : wwork.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(attrs);
    cleanup_child_side();
    if (!ok) {
        CloseHandle(inWr);
        CloseHandle(outRd);
        return fail(last_error("CreateProcess"));
    }

    // Sans job, aucune garantie que l'enfant meure avec son parent : on
    // REFUSE plutôt que de laisser tourner un orphelin potentiel — que la
    // création échoue ou l'affectation.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    bool jobOk = false;
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim{};
        lim.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        jobOk = SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim,
                                        sizeof(lim)) &&
                AssignProcessToJobObject(job, pi.hProcess);
    }
    if (!jobOk) {
        const std::string why = last_error("objet job");
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        if (job) CloseHandle(job);
        CloseHandle(inWr);
        CloseHandle(outRd);
        return fail(why);
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    proc_ = pi.hProcess;
    job_ = job;
    inWr_ = inWr;
    outRd_ = outRd;
    pid_ = pi.dwProcessId;
    return true;
}

bool Child::write_all(const char* data, std::size_t n) {
    if (!inWr_) return false;
    while (n > 0) {
        DWORD w = 0;
        const DWORD chunk = n > 65536 ? 65536 : static_cast<DWORD>(n);
        if (!WriteFile(static_cast<HANDLE>(inWr_), data, chunk, &w, nullptr) || w == 0)
            return false;
        data += w;
        n -= w;
    }
    return true;
}

long Child::read_some(char* buf, std::size_t cap) {
    if (!outRd_) return -1;
    DWORD r = 0;
    const DWORD want = cap > 65536 ? 65536 : static_cast<DWORD>(cap);
    if (!ReadFile(static_cast<HANDLE>(outRd_), buf, want, &r, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return static_cast<long>(r);
}

void Child::close_stdin() { close_h(inWr_); }

void Child::terminate() {
    // Le job tue l'arbre entier : le plugin ET ce qu'il aurait lancé.
    if (job_) TerminateJobObject(static_cast<HANDLE>(job_), 1);
    else if (proc_) TerminateProcess(static_cast<HANDLE>(proc_), 1);
}

bool Child::alive() {
    if (!proc_) return false;
    return WaitForSingleObject(static_cast<HANDLE>(proc_), 0) == WAIT_TIMEOUT;
}

long Child::pid() const { return static_cast<long>(pid_); }

long stdio_read(char* buf, std::size_t cap) {
    DWORD r = 0;
    const DWORD want = cap > 65536 ? 65536 : static_cast<DWORD>(cap);
    if (!ReadFile(GetStdHandle(STD_INPUT_HANDLE), buf, want, &r, nullptr))
        return GetLastError() == ERROR_BROKEN_PIPE ? 0 : -1;
    return static_cast<long>(r);
}

bool stdio_write(const char* data, std::size_t n) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    while (n > 0) {
        DWORD w = 0;
        const DWORD chunk = n > 65536 ? 65536 : static_cast<DWORD>(n);
        if (!WriteFile(h, data, chunk, &w, nullptr) || w == 0) return false;
        data += w;
        n -= w;
    }
    return true;
}

#else // POSIX ---------------------------------------------------------------

extern "C" char** environ;

namespace {

void ignore_sigpipe_once() {
    // Écrire dans le tube d'un enfant mort lève SIGPIPE, dont l'action par
    // défaut TUE le processus — un plugin qui se ferme aurait abattu le
    // host avec lui. On l'ignore : write() rend alors EPIPE, qu'on traite.
    static const bool done = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    (void)done;
}

void close_fd(int& fd) {
    if (fd >= 0) ::close(fd);
    fd = -1;
}

} // namespace

Child::~Child() {
    terminate();
    close_fd(inWr_);
    close_fd(outRd_);
    if (pid_ > 0 && !reaped_) {
        int st = 0;
        ::waitpid(pid_, &st, 0);
    }
}

bool Child::start(const Options& o, std::string* err) {
    auto fail = [&](const std::string& m) {
        if (err) *err = m;
        return false;
    };
    if (o.program.empty()) return fail("programme vide");
    ignore_sigpipe_once();

    int in[2] = {-1, -1}, out[2] = {-1, -1};
    if (::pipe2(in, O_CLOEXEC) != 0) return fail(std::string("pipe2 : ") + std::strerror(errno));
    if (::pipe2(out, O_CLOEXEC) != 0) {
        ::close(in[0]);
        ::close(in[1]);
        return fail(std::string("pipe2 : ") + std::strerror(errno));
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2,
                                     o.stderrPath.empty() ? "/dev/null" : o.stderrPath.c_str(),
                                     O_WRONLY | O_CREAT | O_APPEND, 0600);
#if defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    // Ferme tout descripteur hérité au-delà de 2, y compris ceux qu'une
    // bibliothèque aurait ouverts sans O_CLOEXEC.
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);
#endif
    if (!o.workDir.empty()) posix_spawn_file_actions_addchdir_np(&fa, o.workDir.c_str());

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    // Groupe de processus propre : terminate() pourra tuer le plugin ET
    // tout ce qu'il aurait lancé.
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&at, 0);

    std::vector<std::string> argvS;
    argvS.push_back(o.program);
    for (const auto& a : o.args) argvS.push_back(a);
    std::vector<char*> argv;
    for (auto& s : argvS) argv.push_back(s.data());
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = ::posix_spawnp(&pid, o.program.c_str(), &fa, &at, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    ::close(in[0]);
    ::close(out[1]);
    if (rc != 0) {
        ::close(in[1]);
        ::close(out[0]);
        return fail(std::string("posix_spawn : ") + std::strerror(rc));
    }
    pid_ = pid;
    inWr_ = in[1];
    outRd_ = out[0];
    reaped_ = false;
    return true;
}

bool Child::write_all(const char* data, std::size_t n) {
    if (inWr_ < 0) return false;
    while (n > 0) {
        const ssize_t w = ::write(inWr_, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

long Child::read_some(char* buf, std::size_t cap) {
    if (outRd_ < 0) return -1;
    for (;;) {
        const ssize_t r = ::read(outRd_, buf, cap);
        if (r < 0 && errno == EINTR) continue;
        return static_cast<long>(r);
    }
}

void Child::close_stdin() { close_fd(inWr_); }

void Child::terminate() {
    if (pid_ <= 0 || reaped_) return;
    // Le groupe entier : le plugin et sa descendance.
    ::kill(-pid_, SIGTERM);
    for (int i = 0; i < 20; ++i) {
        int st = 0;
        if (::waitpid(pid_, &st, WNOHANG) == pid_) {
            reaped_ = true;
            ::kill(-pid_, SIGKILL); // descendants éventuels
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ::kill(-pid_, SIGKILL);
    int st = 0;
    if (::waitpid(pid_, &st, 0) == pid_) reaped_ = true;
}

bool Child::alive() {
    if (pid_ <= 0 || reaped_) return false;
    int st = 0;
    const pid_t r = ::waitpid(pid_, &st, WNOHANG);
    if (r == pid_) {
        reaped_ = true;
        return false;
    }
    return r == 0;
}

long Child::pid() const { return static_cast<long>(pid_); }

long stdio_read(char* buf, std::size_t cap) {
    for (;;) {
        const ssize_t r = ::read(0, buf, cap);
        if (r < 0 && errno == EINTR) continue;
        return static_cast<long>(r);
    }
}

bool stdio_write(const char* data, std::size_t n) {
    ignore_sigpipe_once();
    while (n > 0) {
        const ssize_t w = ::write(1, data, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

#endif

} // namespace tl::pipechild

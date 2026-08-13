// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ChildProcess implementation. See the header for the zombie and
// terminate-is-not-a-request traps.

#include "ChildProcess.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#if POM2_HAS_CHILD_PROCESS
#  ifdef _WIN32
#    ifndef WIN32_LEAN_AND_MEAN
#      define WIN32_LEAN_AND_MEAN
#    endif
#    ifndef NOMINMAX
#      define NOMINMAX
#    endif
#    include <windows.h>
#  else
#    include <cerrno>
#    include <csignal>
#    include <sys/stat.h>
#    include <sys/syscall.h>
#    include <sys/types.h>
#    include <sys/wait.h>
#    include <unistd.h>
#  endif
#endif

namespace pom2 {

ChildProcess::~ChildProcess() { stop(); }

#if !POM2_HAS_CHILD_PROCESS

bool ChildProcess::start(const std::string&, const std::vector<std::string>&,
                         const std::string&, std::string& errOut)
{ errOut = "launching helper programs is not available in this build"; return false; }
bool ChildProcess::isRunning() { return false; }
void ChildProcess::stop(int) {}
void ChildProcess::reset() {}
std::string ChildProcess::findOnPath(const std::string&) { return {}; }

#else

// ═════════════════════════════════════════════════════════════════════════
// POSIX
// ═════════════════════════════════════════════════════════════════════════
#ifndef _WIN32

void ChildProcess::reset() { pid_ = -1; }

bool ChildProcess::start(const std::string& exePath,
                         const std::vector<std::string>& args,
                         const std::string& workingDir,
                         std::string& errOut)
{
    if (isRunning()) stop();

    if (exePath.empty()) { errOut = "no helper program configured"; return false; }
    if (::access(exePath.c_str(), X_OK) != 0) {
        errOut = exePath + ": " + std::strerror(errno);
        return false;
    }

    // Build argv BEFORE forking: allocating between fork() and exec() in a
    // multi-threaded process can deadlock on the allocator's lock, and POM2
    // is very much multi-threaded (CPU worker, audio, link workers).
    std::vector<std::string> owned;
    owned.reserve(args.size() + 1);
    owned.push_back(exePath);
    for (const auto& a : args) owned.push_back(a);
    std::vector<char*> argv;
    argv.reserve(owned.size() + 1);
    for (auto& s : owned) argv.push_back(const_cast<char*>(s.c_str()));
    argv.push_back(nullptr);

    // Descriptor ceiling for the child's close loop, queried BEFORE the fork
    // for the same reason argv is built here: sysconf() is not on the
    // async-signal-safe list.
    long maxFd = ::sysconf(_SC_OPEN_MAX);
    if (maxFd < 3 || maxFd > 65536) maxFd = 65536;

    const pid_t pid = ::fork();
    if (pid < 0) { errOut = std::string("fork: ") + std::strerror(errno); return false; }

    if (pid == 0) {
        // Child. Only async-signal-safe calls from here to execv().
        //
        // New process group: POM2 running in a terminal would otherwise pass
        // its Ctrl-C to the helper as well, killing it out from under us and
        // leaving the emulator convinced it is still there.
        ::setpgid(0, 0);
        if (!workingDir.empty()) {
            if (::chdir(workingDir.c_str()) != 0) ::_exit(127);
        }

        // Close every inherited descriptor above stdio.
        //
        // fork() dups the ENTIRE descriptor table, and nothing in POM2 opens
        // its sockets with SOCK_CLOEXEC. Without this loop the helper holds a
        // live copy of the SP-over-SLIP listener on 1985, the AI-control
        // server on 6503 and any SSC telnet listener — and keeps them BOUND
        // after POM2 closes its own copy. "Drop peer" then fails to re-bind
        // with EADDRINUSE, and a Ctrl-C on POM2 (which runs no destructor, so
        // helper_.stop() never fires) leaves an orphan squatting the port for
        // the next session. That is trap 2 in child_process_test.cpp, and the
        // header's "nothing is inherited beyond stdio" promise — the Win32
        // branch keeps it with bInheritHandles=FALSE, this is the POSIX half.
        //
        // close_range() is one syscall for the whole range; the loop is the
        // fallback for kernels older than 5.9. Both are async-signal-safe.
#if defined(__linux__) && defined(SYS_close_range)
        if (::syscall(SYS_close_range, 3u, ~0u, 0u) != 0)
#endif
        {
            for (int fd = 3; fd < static_cast<int>(maxFd); ++fd) ::close(fd);
        }

        ::execv(argv[0], argv.data());
        ::_exit(127);                    // exec failed
    }

    // Set the group from the PARENT too. The child does it as well, and
    // whichever runs first wins — without this, `stop()` can signal the group
    // in the window before the child's own setpgid() lands, and miss it.
    // EACCES simply means the child already exec'd, which is fine.
    ::setpgid(pid, pid);

    pid_      = static_cast<int>(pid);
    path_     = exePath;
    exitCode_ = -1;
    return true;
}

bool ChildProcess::isRunning()
{
    if (pid_ < 0) return false;

    // waitpid, NOT kill(pid, 0): a child that has exited but not been reaped
    // still answers kill(), so the naive check reports it alive forever AND
    // leaves a zombie behind.
    int   status = 0;
    const pid_t r = ::waitpid(pid_, &status, WNOHANG);
    if (r == 0) return true;             // still running
    if (r < 0) {
        if (errno == EINTR) return true; // ask again next tick
        reset();                         // ECHILD: not ours (already reaped)
        return false;
    }
    exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    reset();
    return false;
}

void ChildProcess::stop(int graceMs)
{
    if (pid_ < 0) return;

    // Signal the whole PROCESS GROUP, not just the child.
    //
    // A helper that spawns its own children — a launcher script, or a shell
    // wrapper — leaves them running when only the direct child is killed, and
    // they inherit our stdout pipe. That is not theoretical: it made this
    // file's own test hang under ctest, which waits for every process holding
    // the pipe, while the same test passed when run by hand. In production it
    // would mean a stray FujiNet still holding the loopback port after POM2
    // "stopped" it. The child is its own group leader (setpgid above), so the
    // negated pid addresses exactly its group and nothing else.
    ::kill(-pid_, SIGTERM);

    // Give it the grace period to shut down cleanly — a FujiNet flushing an
    // SD-card image should be allowed to finish.
    const int stepMs = 25;
    for (int waited = 0; waited < graceMs; waited += stepMs) {
        if (!isRunning()) return;
        ::usleep(static_cast<useconds_t>(stepMs) * 1000);
    }

    if (pid_ < 0) return;
    ::kill(-pid_, SIGKILL);
    // Reap the corpse; without this the zombie outlives us until POM2 exits.
    int status = 0;
    while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {}
    exitCode_ = -1;
    reset();
}

std::string ChildProcess::findOnPath(const std::string& name)
{
    auto executable = [](const std::string& p) {
        struct stat st{};
        return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode) &&
               ::access(p.c_str(), X_OK) == 0;
    };

    if (name.find('/') != std::string::npos)
        return executable(name) ? name : std::string{};

    if (const char* path = std::getenv("PATH")) {
        std::string p(path);
        size_t start = 0;
        while (start <= p.size()) {
            const size_t end = p.find(':', start);
            const std::string dir =
                p.substr(start, end == std::string::npos ? std::string::npos
                                                         : end - start);
            if (!dir.empty()) {
                const std::string cand = dir + "/" + name;
                if (executable(cand)) return cand;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
    }

    // The places a FujiNet desktop build actually lands when it was not
    // installed to a PATH directory.
    const char* home = std::getenv("HOME");
    std::vector<std::string> extra = {
        "/usr/local/bin/" + name,
        "/opt/" + name + "/" + name,
        "/app/bin/" + name,                       // inside a flatpak
    };
    if (home) {
        extra.push_back(std::string(home) + "/.local/bin/" + name);
        extra.push_back(std::string(home) + "/bin/" + name);
    }
    for (const auto& c : extra)
        if (executable(c)) return c;
    return {};
}

// ═════════════════════════════════════════════════════════════════════════
// Win32
// ═════════════════════════════════════════════════════════════════════════
#else

namespace {
HANDLE H(void* h) { return static_cast<HANDLE>(h); }

std::wstring utf8ToWide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                      s.data(), static_cast<int>(s.size()),
                                      nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::string wideToUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                      s.data(), static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                        static_cast<int>(s.size()), out.data(), n,
                        nullptr, nullptr);
    return out;
}
} // namespace

void ChildProcess::reset()
{
    if (handle_) CloseHandle(H(handle_));
    if (job_) CloseHandle(H(job_));
    handle_ = nullptr;
    job_ = nullptr;
}

bool ChildProcess::start(const std::string& exePath,
                         const std::vector<std::string>& args,
                         const std::string& workingDir,
                         std::string& errOut)
{
    if (isRunning()) stop();
    if (exePath.empty()) { errOut = "no helper program configured"; return false; }

    // Win32 takes ONE command line, not an argv array, so every argument has
    // to be quoted the way the CRT will re-split it.
    auto quote = [](const std::wstring& a) {
        if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos)
            return a;
        std::wstring q = L"\"";
        std::size_t slashes = 0;
        for (wchar_t c : a) {
            if (c == L'\\') { ++slashes; continue; }
            if (c == L'"') {
                q.append(slashes * 2 + 1, L'\\');
                q += L'"';
            } else {
                q.append(slashes, L'\\');
                q += c;
            }
            slashes = 0;
        }
        // Backslashes immediately before the closing quote must be doubled,
        // otherwise the CRT treats the last one as escaping that quote.
        q.append(slashes * 2, L'\\');
        q += L'"';
        return q;
    };
    const std::wstring wideExe = utf8ToWide(exePath);
    const std::wstring wideDir = utf8ToWide(workingDir);
    if (wideExe.empty() || (!workingDir.empty() && wideDir.empty())) {
        errOut = "helper path is not valid UTF-8";
        return false;
    }
    std::wstring cmd = quote(wideExe);
    for (const auto& a : args) {
        const std::wstring wideArg = utf8ToWide(a);
        if (!a.empty() && wideArg.empty()) {
            errOut = "helper argument is not valid UTF-8";
            return false;
        }
        cmd += L' ';
        cmd += quote(wideArg);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    std::vector<wchar_t> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back(L'\0');

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        errOut = "CreateJobObject failed (" +
                 std::to_string(GetLastError()) + ")";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        errOut = "SetInformationJobObject failed (" +
                 std::to_string(GetLastError()) + ")";
        CloseHandle(job);
        return false;
    }

    // CREATE_NO_WINDOW: the helper is a console program, and POM2 is not —
    // without this every start pops a console window on the user's desktop.
    // CREATE_NEW_PROCESS_GROUP is the counterpart of setpgid() above.
    if (!CreateProcessW(wideExe.c_str(), mutableCmd.data(), nullptr, nullptr,
                        FALSE,
                        CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP |
                            CREATE_SUSPENDED,
                        nullptr,
                        workingDir.empty() ? nullptr : wideDir.c_str(),
                        &si, &pi)) {
        const DWORD e = GetLastError();
        errOut = exePath + ": CreateProcess failed (" + std::to_string(e) + ")";
        CloseHandle(job);
        return false;
    }
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        const DWORD e = GetLastError();
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        errOut = exePath + ": AssignProcessToJobObject failed (" +
                 std::to_string(e) + ")";
        return false;
    }
    if (ResumeThread(pi.hThread) == static_cast<DWORD>(-1)) {
        const DWORD e = GetLastError();
        TerminateJobObject(job, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job);
        errOut = exePath + ": ResumeThread failed (" +
                 std::to_string(e) + ")";
        return false;
    }
    CloseHandle(pi.hThread);
    handle_   = pi.hProcess;
    job_      = job;
    path_     = exePath;
    exitCode_ = -1;
    return true;
}

bool ChildProcess::isRunning()
{
    if (!handle_) return false;
    DWORD code = 0;
    if (!GetExitCodeProcess(H(handle_), &code)) { reset(); return false; }
    if (code == STILL_ACTIVE) return true;
    exitCode_ = static_cast<int>(code);
    reset();
    return false;
}

void ChildProcess::stop(int graceMs)
{
    if (!handle_) return;
    // Win32 has no SIGTERM for a windowless console child; the process group
    // Ctrl-Break is the closest thing, and TerminateProcess is the fallback.
    //
    // The event usually does NOT get through: CREATE_NO_WINDOW gives the child
    // its own hidden console rather than ours, and GenerateConsoleCtrlEvent
    // only reaches a group sharing the CALLER's console. Ignoring the return
    // value meant every stop paid the full grace period on the UI thread
    // before the hard kill, so check it — a failed signal means nobody is
    // going to exit politely and there is nothing to wait for.
    const BOOL signalled =
        GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, GetProcessId(H(handle_)));

    bool exited = false;
    if (signalled) {
        // Poll in short steps like the POSIX branch, so a helper that does
        // honour the break does not cost the whole grace period either.
        const DWORD stepMs = 25;
        for (DWORD waited = 0; waited < static_cast<DWORD>(graceMs); waited += stepMs) {
            if (WaitForSingleObject(H(handle_), stepMs) == WAIT_OBJECT_0) {
                exited = true;
                break;
            }
        }
    }
    if (!exited) {
        if (job_) TerminateJobObject(H(job_), 1);
        else TerminateProcess(H(handle_), 1);
    }
    WaitForSingleObject(H(handle_), 1000);
    exitCode_ = -1;
    reset();
}

std::string ChildProcess::findOnPath(const std::string& name)
{
    const std::string exe = (name.size() > 4 &&
                             name.compare(name.size() - 4, 4, ".exe") == 0)
                                ? name : name + ".exe";
    const std::wstring wideExe = utf8ToWide(exe);
    if (wideExe.empty()) return {};
    const DWORD needed = SearchPathW(nullptr, wideExe.c_str(), nullptr,
                                     0, nullptr, nullptr);
    if (needed == 0) return {};
    std::vector<wchar_t> buf(static_cast<size_t>(needed) + 1, L'\0');
    wchar_t* filePart = nullptr;
    const DWORD n = SearchPathW(nullptr, wideExe.c_str(), nullptr,
                                static_cast<DWORD>(buf.size()), buf.data(),
                                &filePart);
    if (n > 0 && n < buf.size()) return wideToUtf8(std::wstring(buf.data(), n));
    return {};
}

#endif // _WIN32
#endif // POM2_HAS_CHILD_PROCESS

} // namespace pom2

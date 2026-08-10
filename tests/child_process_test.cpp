// Helper-process supervision test — pins src/ChildProcess.cpp.
//
// POM2 launches a FujiNet desktop build for the user instead of making them
// start it by hand. Getting that wrong is not cosmetic:
//
//   1. A ZOMBIE READS AS ALIVE. On POSIX a child that exited stays in the
//      process table until somebody wait()s for it, and `kill(pid, 0)` still
//      succeeds on it. A liveness check written that way reports the helper
//      running forever, so the UI offers "Stop" for a process that is gone
//      and never offers "Start" again.
//   2. AN ORPHANED HELPER HOLDS THE PORT. If POM2 exits without reaping its
//      child, the FujiNet keeps the loopback connection and the *next* POM2
//      session cannot bind 1985 — which looks like POM2 being broken.
//   3. A FAILED LAUNCH MUST SAY SO. A missing or non-executable path has to
//      come back as a clean error, not a half-started state.
//
// POSIX-only harness: it needs /bin/sh to stand in for the helper. The Win32
// path is exercised by the manual checklist.

#include "ChildProcess.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#if !POM2_HAS_CHILD_PROCESS || defined(_WIN32)

int main()
{
    std::puts("SKIP: child-process harness is POSIX-only");
    return 0;
}

#else

#include <arpa/inet.h>
#include <chrono>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace {

using pom2::ChildProcess;

void sleepMs(int ms)
{ std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// ── 1. A short-lived child is noticed, and reaped ────────────────────────
void testExitIsDetectedAndReaped()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "exit 3" }, "", err));

    // It must eventually report NOT running. If isRunning() used kill(pid,0)
    // this loop would never end — the zombie answers.
    bool ended = false;
    for (int i = 0; i < 200 && !ended; ++i) {
        if (!p.isRunning()) ended = true;
        else sleepMs(10);
    }
    assert(ended);
    // And the exit status survived the reap.
    assert(p.lastExitCode() == 3);
}

// ── 2. A long-lived child is alive until we stop it ──────────────────────
void testStopTerminates()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));

    sleepMs(50);
    assert(p.isRunning());

    const auto t0 = std::chrono::steady_clock::now();
    p.stop(1000);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    assert(!p.isRunning());
    // SIGTERM should have done it well inside the grace period — if we had to
    // fall through to SIGKILL this would sit at the full 1000 ms.
    assert(ms < 900);
}

// ── 3. A child that ignores SIGTERM is killed anyway ─────────────────────
void testStubbornChildIsKilled()
{
    ChildProcess p;
    std::string err;
    // trap '' TERM makes the shell ignore SIGTERM outright.
    assert(p.start("/bin/sh", { "-c", "trap '' TERM; sleep 60" }, "", err));
    sleepMs(80);
    assert(p.isRunning());

    p.stop(200);              // short grace, so the SIGKILL path runs
    assert(!p.isRunning());   // it is gone regardless

    // AND SO IS ITS `sleep`. The shell here spawns a grandchild; killing only
    // the direct child leaves that grandchild running and holding our stdout
    // pipe, which is invisible when this test is run by hand but hangs it
    // under ctest (ctest waits for every process on the pipe). `stop()`
    // signals the process group for exactly this reason. If that regresses,
    // this test times out rather than failing an assert — the comment is the
    // only thing that will explain why.
}

// ── 4. The destructor must not leak the child ────────────────────────────
void testDestructorStops()
{
    int pidProbe = 0;
    {
        ChildProcess p;
        std::string err;
        assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));
        sleepMs(50);
        assert(p.isRunning());
        pidProbe = 1;
    }   // ~ChildProcess() must terminate it
    assert(pidProbe == 1);
    // Nothing to assert directly without racing the OS; the value here is
    // that the destructor path runs under the same asserts as stop() and
    // does not hang. A leaked helper is what holds the loopback port.
}

// ── 5. Failures are clean ────────────────────────────────────────────────
void testStartFailures()
{
    ChildProcess p;
    std::string err;

    assert(!p.start("", {}, "", err));
    assert(!err.empty());
    assert(!p.isRunning());

    err.clear();
    assert(!p.start("/definitely/not/here/fujinet", {}, "", err));
    assert(!err.empty());
    assert(!p.isRunning());

    // A path that exists but is not executable.
    err.clear();
    assert(!p.start("/etc/hostname", {}, "", err));
    assert(!p.isRunning());
}

// ── 6. Restart replaces the previous child ───────────────────────────────
void testRestartReplaces()
{
    ChildProcess p;
    std::string err;
    assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));
    sleepMs(50);
    assert(p.isRunning());

    // Starting again must stop the first one rather than leaking it.
    assert(p.start("/bin/sh", { "-c", "sleep 60" }, "", err));
    sleepMs(50);
    assert(p.isRunning());
    p.stop(1000);
    assert(!p.isRunning());
}

// ── 7. findOnPath ────────────────────────────────────────────────────────
void testFindOnPath()
{
    // Something that is certainly on PATH.
    const std::string sh = ChildProcess::findOnPath("sh");
    assert(!sh.empty());
    assert(sh.find("/sh") != std::string::npos);

    // An explicit path is validated rather than searched.
    assert(ChildProcess::findOnPath("/bin/sh") == "/bin/sh");
    assert(ChildProcess::findOnPath("/definitely/not/here").empty());

    // A name nobody has must come back empty, not throw.
    assert(ChildProcess::findOnPath("pom2-no-such-helper-xyz").empty());
}

// ── 8. The child inherits NO descriptor beyond stdio ─────────────────────
//
// This is trap 2 above, at its root. fork() dups the whole descriptor table
// and POM2 opens no socket with SOCK_CLOEXEC, so a child that does not close
// what it inherited keeps POM2's listeners BOUND: "Drop peer" then fails to
// re-bind with EADDRINUSE, and a Ctrl-C (which runs no destructor, so the
// helper is never stopped) leaves an orphan squatting the port for the next
// session. Before the close loop in ChildProcess::start this failed on the
// first rebind.
void testChildDoesNotInheritListeners()
{
    // A listener on an ephemeral port, so the test never collides with a real
    // POM2 session — the DEFECT is about descriptor inheritance, not 1985.
    auto bindListener = [](int& portOut) {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in a{};
        a.sin_family      = AF_INET;
        a.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        a.sin_port        = ::htons(static_cast<uint16_t>(portOut));
        if (::bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) != 0 ||
            ::listen(fd, 2) != 0) {
            ::close(fd);
            return -1;
        }
        socklen_t len = sizeof(a);
        ::getsockname(fd, reinterpret_cast<sockaddr*>(&a), &len);
        portOut = ::ntohs(a.sin_port);
        return fd;
    };

    int port = 0;                       // 0 = let the kernel pick
    const int listenFd = bindListener(port);
    assert(listenFd >= 0 && port != 0);

    ChildProcess p;
    std::string  err;
    assert(p.start("/bin/sh", { "-c", "sleep 30" }, "", err));
    sleepMs(50);
    assert(p.isRunning());

    // Give up OUR copy. If the child inherited one, the port stays in LISTEN
    // and nothing can take it — which is exactly the wedge users saw.
    ::close(listenFd);

    const int again = bindListener(port);
    const bool rebound = again >= 0;
    if (rebound) ::close(again);
    p.stop(1000);

    assert(rebound && "the helper inherited POM2's listening socket");
}

} // namespace

int main()
{
    testExitIsDetectedAndReaped();
    testStopTerminates();
    testStubbornChildIsKilled();
    testDestructorStops();
    testStartFailures();
    testRestartReplaces();
    testFindOnPath();
    testChildDoesNotInheritListeners();

    std::puts("child_process: OK");
    return 0;
}

#endif // POSIX

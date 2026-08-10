// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ChildProcess — launch and supervise one helper program, POSIX and Win32
// behind one API.
//
// POM2 owns the child for its whole life: it starts it, can tell whether it is
// still alive, and reaps it on shutdown. Nothing is inherited by the child
// beyond stdio, and nothing survives POM2 exiting — a helper left running
// after the emulator quits would hold the loopback port that the next session
// wants to listen on.
//
// Written for the FujiNet helper (a FujiNet desktop build POM2 can start for
// the user instead of making them run it by hand), but there is nothing
// FujiNet-specific here.
//
// ── The two traps ─────────────────────────────────────────────────────────
//
//   1. ZOMBIES. On POSIX a child that exits stays in the process table until
//      somebody wait()s for it. `isRunning()` therefore does a
//      `waitpid(WNOHANG)` rather than a bare `kill(pid, 0)` — the latter
//      reports a dead-but-unreaped child as alive, forever.
//   2. TERMINATE IS NOT A REQUEST. `stop()` asks politely first (SIGTERM /
//      console-less TerminateProcess is all Win32 offers), waits a grace
//      period, and only then kills. A FujiNet flushing its SD card image
//      deserves the chance to finish.
//
// Not available under Emscripten (no processes in a browser):
// POM2_HAS_CHILD_PROCESS is 0 there and `start()` fails cleanly.

#ifndef POM2_CHILD_PROCESS_H
#define POM2_CHILD_PROCESS_H

#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#  define POM2_HAS_CHILD_PROCESS 0
#else
#  define POM2_HAS_CHILD_PROCESS 1
#endif

namespace pom2 {

class ChildProcess
{
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess(const ChildProcess&)            = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;

    /// Launch `exePath` with `args` (argv[0] is supplied automatically).
    /// `workingDir` empty = inherit POM2's. Returns false with a
    /// human-readable reason in `errOut`; a process already running is
    /// stopped first.
    bool start(const std::string& exePath,
               const std::vector<std::string>& args,
               const std::string& workingDir,
               std::string& errOut);

    /// Alive? Reaps the child if it has exited, so this is also what keeps
    /// zombies out of the process table — call it periodically.
    bool isRunning();

    /// SIGTERM, wait `graceMs`, then SIGKILL. Safe to call when nothing is
    /// running. Blocks for at most `graceMs`.
    void stop(int graceMs = 2000);

    /// Exit status of the last child that finished, or -1 if it was killed /
    /// never ran. Only meaningful once `isRunning()` has returned false.
    int  lastExitCode() const { return exitCode_; }

    const std::string& path() const { return path_; }

    /// Look for `name` on PATH and in the usual install locations, returning
    /// the first hit or "" — so the UI can offer a sensible default without
    /// the user hunting for the binary.
    static std::string findOnPath(const std::string& name);

private:
    void reset();

#if POM2_HAS_CHILD_PROCESS
#  ifdef _WIN32
    void* handle_ = nullptr;      ///< HANDLE, kept void* to keep windows.h out
#  else
    int   pid_ = -1;
#  endif
#endif
    std::string path_;
    int         exitCode_ = -1;
};

} // namespace pom2

#endif // POM2_CHILD_PROCESS_H

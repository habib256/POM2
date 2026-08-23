// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Debugger_ImGui — the window you interrogate a running Apple II from.
//
// Registers, a disassembly that follows the PC, a breakpoint gutter you click,
// and the five verbs that matter: Run, Stop, Step, Step Over, Run To Cursor.
// The stop banner says WHY the machine is stopped, because "it stopped" and
// "it stopped because you asked it to break at $C600" are different facts and
// only one of them is useful.
//
// Everything lives here. `MainWindow` owns the object and calls `render()`;
// it holds no debugger state and makes no debugger decisions, which is the
// rule the file-size ratchet (tools/check_file_sizes.sh) exists to keep.
//
// ── Locking ──────────────────────────────────────────────────────────────
// The panel runs on the UI thread and every emulator read or write it makes
// goes through `stateMutex` — the debugger's bitmaps are consulted by the CPU
// worker inside that same lock (Debugger.h § Threading). The snapshot pattern
// is the one the rest of POM2's panels use: take a VALUE copy of everything
// under the lock, release it, then draw. Holding it across ImGui calls would
// freeze the machine for the length of a repaint.

#ifndef POM2_DEBUGGER_IMGUI_H
#define POM2_DEBUGGER_IMGUI_H

#include "Debugger.h"

#include <cstdint>
#include <string>
#include <vector>

class EmulationController;

namespace pom2 {

class Debugger_ImGui
{
public:
    /// Draw the window. `open` is the caller's visibility flag, wired to the
    /// window's close button in the usual ImGui way; when false this returns
    /// immediately having taken no lock.
    void render(EmulationController& ctrl, bool* open);

private:
    /// A value snapshot of everything the panel draws, taken under the lock
    /// in one go. Nothing below is a pointer into live emulator state — that
    /// was the 2026-08-02 disk-path race, and it applies here for the same
    /// reason (see tests/disk_path_snapshot_test.cpp).
    struct Snapshot {
        uint8_t  a = 0, x = 0, y = 0, p = 0, sp = 0;
        uint16_t pc = 0;
        bool     cmos    = false;
        bool     running = false;
        bool     halted  = false;
        Debugger::Hit hit{};
        std::vector<uint16_t> breakpoints;
        std::vector<Debugger::Watch> watchpoints;
        /// 64 KiB of main RAM, copied so the disassembly can be laid out
        /// after the lock is released.
        std::vector<uint8_t> memory;
    };

    void drawControls(EmulationController& ctrl, const Snapshot& snap);
    void drawRegisters(const Snapshot& snap);
    void drawDisassembly(EmulationController& ctrl, const Snapshot& snap);
    void drawBreakpointList(EmulationController& ctrl, const Snapshot& snap);
    void drawWatchpointList(EmulationController& ctrl, const Snapshot& snap);
    void drawStopBanner(const Snapshot& snap);

    /// Disassembly origin. Follows the PC unless the user has scrolled away.
    uint16_t  viewAddr_    = 0;
    bool      followPc_    = true;
    /// Address typed into the "add breakpoint" box, as text so a half-typed
    /// value does not jump the view.
    char      bpEntry_[8]  = {0};
    /// Same, for the watchpoint box. Separate storage so typing in one does
    /// not clear the other.
    char      wpEntry_[8]  = {0};
    /// The address the user last clicked in the disassembly — the target for
    /// Run To Cursor.
    uint16_t  cursorAddr_  = 0;
    bool      cursorValid_ = false;
};

} // namespace pom2

#endif // POM2_DEBUGGER_IMGUI_H

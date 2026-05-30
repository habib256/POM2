// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Toolbar_ImGui — quick-access button bar pinned just below the menu bar.
//
// Surfaces the actions that the user reaches for many times per session
// (power-cycle, reset, pause, step, speed, profile switch, screenshot,
// disk insert / eject, memory viewer) so they stop being buried in
// three different menus. Same snapshot+Result-of-user-actions pattern
// as the other `*_ImGui` panels: the host (`MainWindow`) builds a
// `Snapshot` under `stateMutex`, the toolbar reads it (no live emulator
// touch) and returns a `Result` for the host to apply.
//
// Layout: tightly packed icon buttons, one row, Font Awesome glyphs when
// `fa-solid-900.ttf` is loaded (text fallback otherwise). Anchored via
// `SetNextWindowPos` at `(0, menuBarHeight)` with NoTitleBar / NoMove /
// NoResize so it can't be dragged out of the way; the user toggles
// visibility from `Window → Toolbar`.

#ifndef POM2_TOOLBAR_IMGUI_H
#define POM2_TOOLBAR_IMGUI_H

#include "CharRomCatalog.h"
#include "SystemProfile.h"

#include <cstdint>

namespace pom2 {

class Toolbar_ImGui
{
public:
    struct Snapshot {
        bool          isRunning          = false;
        bool          isStopped          = false;   // CPU paused (vs single-step ready)
        int           cyclesPerFrame     = 17045;
        bool          memoryGridVisible  = false;
        SystemProfile activeProfile      = SystemProfile::AppleIIPlus;
        // Enable / disable hint for the disk button(s).
        bool          hasPrimaryDiskCard = false;
        // True when the display is currently in a monochrome phosphor mode
        // (drives the color/mono toggle button's icon tint + tooltip).
        bool          displayIsMono      = false;
        // Active character-generator ROM locale. The dropdown only
        // surfaces entries that fit the active profile (see
        // charRomFitsProfile) — switching is hot, no cold reset needed
        // because Apple2Display re-reads `mem.charRom()` every frame.
        CharRomLocale charRomLocale      = CharRomLocale::ProfileDefault;
    };

    struct Result {
        bool requestColdBoot         = false;   // wipe RAM + boot
        bool requestSoftReset        = false;   // F11 / Ctrl-Reset
        bool requestHardReset        = false;   // F12
        bool requestPauseToggle      = false;   // Run ↔ Stopped
        bool requestStep             = false;   // single-instruction step
        bool requestScreenshot       = false;
        bool requestInsertDisk       = false;   // open Insert-disk popup
        bool requestMemoryGridToggle = false;
        bool requestAbout            = false;   // open the About dialog
        bool requestMonoColorToggle  = false;   // flip color ↔ monochrome
        // -1 = no change. Set to the new cyclesPerFrame on speed dropdown
        // click; the host applies it via `EmulationController::setCyclesPerFrame`.
        int           setCyclesPerFrame   = -1;
        bool          setProfileRequested = false;
        SystemProfile setProfile          {};
        // Char ROM dropdown — host swaps via Memory::loadCharRom(path)
        // (or re-runs the profile's probe order for ProfileDefault).
        bool          setCharRomRequested = false;
        CharRomLocale setCharRomLocale    {};
    };

    /// Render the toolbar. `menuBarHeight` positions the window just
    /// below the main menu bar (`ImGui::GetFrameHeight()` at the call
    /// site). `open` mirrors the user's Window menu toggle.
    Result render(bool& open, float menuBarHeight, const Snapshot& snap);
};

} // namespace pom2

#endif // POM2_TOOLBAR_IMGUI_H

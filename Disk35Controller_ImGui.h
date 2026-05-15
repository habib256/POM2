// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Disk35Controller_ImGui — //c+ Sony 3.5" drives status panel. Shows
// both internal + external 3.5" slots side-by-side, the currently
// inserted 800K image (if any), the head track and side, and exposes
// Mount / Eject buttons.
//
// Mirrors `DiskController_ImGui` for 5.25" and `HdvController_ImGui`
// for ProDOS HDV — same read-only-snapshot + FrameResult-of-user-
// actions pattern. The host (MainWindow) builds the snapshot under
// `stateMutex` so the panel never touches live emulator state.

#ifndef POM2_DISK35_CONTROLLER_IMGUI_H
#define POM2_DISK35_CONTROLLER_IMGUI_H

#include <array>
#include <string>
#include <vector>

namespace pom2 {

class Disk35Controller_ImGui
{
public:
    struct LibraryEntry {
        std::string displayName;   // e.g. "ProDOS_2_4_2.po"
        std::string fullPath;      // e.g. "../disks35/ProDOS_2_4_2.po"
    };

    struct DriveSnapshot {
        bool        diskLoaded         = false;
        bool        motorOn            = false;
        int         track              = 0;       // 0..79
        bool        side1              = false;   // false = head 0
        bool        writeProtected     = true;
        std::string diskPath;
        std::string lastError;        // last failed mount, if any
    };

    struct PanelSnapshot {
        // Two drives — index 0 = internal (the //c+ on-board 3.5"),
        // index 1 = external (daisy-chained 3.5" #2).
        std::array<DriveSnapshot, 2> drives{};
        bool                         supportedByProfile = false;
        std::vector<LibraryEntry>    library;
    };

    struct FrameResult {
        // Per-drive eject request — `requestEject[i]` true = eject drive i.
        std::array<bool, 2> requestEject{};
        // Path the user picked from the library + drive index. When
        // empty, no library action this frame.
        std::string         requestMountPath;
        int                 requestMountDrive = 0;
        bool                openMountDialog   = false;
        int                 openMountDialogForDrive = 0;
    };

    FrameResult render(const char*          title,
                       bool&                open,
                       const PanelSnapshot& snap);

    // Mount-dialog state — same convention as the other panels. When
    // `mountDialogOpen` is true, MainWindow renders the popup and
    // resolves the chosen path against `mountDialogForDrive`.
    bool        mountDialogOpen      = false;
    int         mountDialogForDrive  = 0;
    std::string dialogPath;
};

} // namespace pom2

#endif // POM2_DISK35_CONTROLLER_IMGUI_H

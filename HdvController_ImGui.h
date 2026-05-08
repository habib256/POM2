// POM2 Apple II Emulator
// Copyright (C) 2026
//
// HdvController_ImGui — ProDOS hard-disk (slot 5) status panel. Mirrors
// DiskController_ImGui in spirit: a Library list of .hdv / .2mg images
// that one-click "mount + cold boot" via $C500. Read-only by design — a
// future write-back path can plug into the same UI without changing the
// dispatch contract.

#ifndef POM2_HDV_CONTROLLER_IMGUI_H
#define POM2_HDV_CONTROLLER_IMGUI_H

#include <cstddef>
#include <string>
#include <vector>

namespace pom2 {

class HdvController_ImGui
{
public:
    struct LibraryEntry {
        std::string displayName;   // e.g. "Total Replay v5.2.hdv"
        std::string fullPath;      // e.g. "../hdv/Total Replay v5.2.hdv"
    };

    struct DriveSnapshot {
        bool        imageLoaded = false;
        std::string imagePath;
        size_t      blockCount  = 0;
        std::vector<LibraryEntry> library;
    };

    struct FrameResult {
        bool        requestMountDialog = false;
        bool        requestEject       = false;
        bool        requestBoot        = false;     // jump PC to $C500
        // Single-click library entry: host mounts the image AND triggers
        // a cold boot through the slot-5 ROM.
        std::string requestMountAndBoot;
    };

    /// Draw the panel. Called every frame from MainWindow::render().
    /// Returns user actions for the host to apply (mount dialog goes
    /// through MainWindow so the file path widget is in one place).
    FrameResult render(const char*          title,
                       bool&                open,
                       const DriveSnapshot& snap);
};

} // namespace pom2

#endif // POM2_HDV_CONTROLLER_IMGUI_H

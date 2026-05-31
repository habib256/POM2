// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// FloppyEmu_ImGui — the on-screen face of the BMOW Floppy Emu: a stylised
// 128x64 blue-on-black OLED plus the device's three hardware buttons
// (PREV / NEXT / SELECT). Two OLED views: the SD-card File Explorer (default,
// with an optional Favorites menu) and the Settings → Disk Emulation Mode
// list. The mode is always shown in the OLED header, mirroring the device's
// title screen.
//
// Pure data-in / actions-out, like the other *_ImGui panels: MainWindow owns
// the FloppyEmuDevice + the controller cards, builds the snapshot, and applies
// the returned actions (navigate / mount / eject / change mode). The panel
// only owns transient UI state (which view is showing + the list cursor).

#ifndef POM2_FLOPPY_EMU_IMGUI_H
#define POM2_FLOPPY_EMU_IMGUI_H

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class FloppyEmu_ImGui
{
public:
    struct Item {
        std::string label;     // e.g. "GAMES" or "TOTAL_REPLAY.2mg"
        std::string sublabel;  // e.g. "32M", "800K", "DIR"
        bool        isDir = false;
        bool        isUp  = false;
    };

    struct Snapshot {
        std::string modeLabel;          // current emulation mode (OLED header)

        // Browser view
        std::string       dirLabel;     // e.g. "SD:/GAMES"
        std::vector<Item> items;        // listing OR favorites
        bool              favoritesActive    = false;  // showing favdisks
        bool              favoritesAvailable = false;  // favdisks.txt present
        bool              sdPresent          = true;   // SD root exists
        std::string       sdRootDisplay;               // host path of the SD folder

        // Host-side status (rendered under the OLED, not on it)
        bool        controllerReady = true;  // matching controller present
        std::string controllerHint;          // shown when !controllerReady
        std::string insertedLabel;           // current media ("" = none)
        std::string statusLine;              // last mount/eject/error message

        // Settings view
        std::vector<std::string> modeOptions;     // mode labels
        int                      currentModeIndex = 0;
    };

    struct Result {
        int  activateIndex = -1;   // SELECT pressed on browser items[idx]
        int  setModeIndex  = -1;   // SELECT pressed on settings modeOptions[idx]
        bool toggleFavorites = false;
        bool requestEject     = false;
        bool requestConfigureController = false;  // user asked to auto-add it
    };

    Result render(const char* title, bool& open, const Snapshot& snap);

private:
    bool showSettings_   = false;
    int  browseCursor_   = 0;
    int  settingsCursor_ = 0;
    // Last-seen listing identity; browseCursor_ re-homes to 0 when it changes
    // (dir enter/leave, Favorites toggle, mode switch).
    std::string lastDirLabel_;
    bool        lastFavActive_ = false;
    int         lastModeIndex_ = -1;
};

} // namespace pom2

#endif // POM2_FLOPPY_EMU_IMGUI_H

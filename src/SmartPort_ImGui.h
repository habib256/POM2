// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SmartPort_ImGui — configuration panel for the slot-plugged SmartPortCard
// (Liron-class). One row per unit (kMaxUnits=2): type selector
// ([empty] / 3.5" 800K / ProDOS HDV), mount + eject buttons, write-back
// toggle, and an inline path field. The panel is pure data-in / actions-
// out — StorageCoordinator resolves the card under the state lock and
// applies the requested command without exposing card pointers to ImGui.
//
// Why a separate panel from `Disk35Controller_ImGui`: the existing
// 3.5" panel drives the //c+ on-board pair of `Disk35Image` instances
// owned by `EmulationController`. Slot-plugged Liron-class cards now
// own their units independently (different concerns: on-board hub vs
// slot-card chain), so they need their own UI surface.

#ifndef POM2_SMARTPORT_IMGUI_H
#define POM2_SMARTPORT_IMGUI_H

#include <array>
#include <cstdint>
#include <string>

namespace pom2 {

class SmartPort_ImGui
{
public:
    // ── Input snapshot the host (MainWindow) populates each frame ─────
    struct UnitSnapshot {
        // "" = empty slot. Non-empty = SmartPortUnit::kindKey of the
        // currently-installed unit ("35" / "hdv" / …).
        std::string kind;
        std::string kindLabel;
        std::string path;
        std::string lastError;
        uint32_t    blockCount       = 0;
        bool        loaded           = false;
        bool        writeProtected   = false;
        bool        writeBackEnabled = false;
    };

    struct CardSnapshot {
        int  slot     = 0;
        bool plugged  = false;
        std::array<UnitSnapshot, 2> units{};
    };

    // ── Output: actions the user requested this frame ──────────────────
    struct UnitAction {
        // Replace the unit's type. Non-empty string = kindKey of the
        // new unit (creates it via makeSmartPortUnit). Empty string +
        // `clearType=true` = remove the unit (set to empty slot).
        // `clearType=false` AND empty `setType` = no type change.
        std::string setType;
        bool        clearType        = false;
        std::string mountPath;       // non-empty = mount this image
        bool        eject            = false;
        bool        writeBackChanged = false;
        bool        writeBackOn      = false;
    };

    struct Result {
        std::array<UnitAction, 2> units{};
    };

    /// Render the panel. `open` is the host's bool toggle.
    /// Returns the actions the user requested this frame.
    Result render(const char* title, bool& open, const CardSnapshot& snap);

private:
    // Per-unit mount-dialog state. ImGui input box buffers can't live
    // on the stack across frames, so we keep them here.
    std::array<char, 256> pathBufs_[2] = {};
};

} // namespace pom2

#endif // POM2_SMARTPORT_IMGUI_H

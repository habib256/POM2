// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SlotManager_ImGui — the consolidated "control center" for the expansion
// bus. One window shows every slot (1-7, plus the AUX 80-column row on
// IIe-class machines), the card assigned to each (a dropdown, or a locked
// built-in badge), and — for any card that exposes mountable media bays
// (MountableMediaCard: HDV, CFFA, SmartPort units) — inline mount / eject /
// write-back / type-select / boot controls.
//
// Why one panel instead of N per-card windows: the host bus is multi-
// instance (two block cards, several SmartPort cards), but the legacy
// per-card panels each assumed a single global pointer. The Slot Manager
// is built from a SlotBus *enumeration*, so it stays correct no matter how
// many cards of a kind are plugged, and gives the user a single place to
// see and drive the whole machine. The detailed per-card panels remain for
// deep state (track LEDs, motor, library browsing) and are reachable via
// the "Open detailed panel" buttons here.
//
// Pure data-in / actions-out, same contract as the other *_ImGui panels:
// MainWindow builds the snapshot under the state mutex and applies the
// returned actions (mount/eject/persist/restart) itself.

#ifndef POM2_SLOT_MANAGER_IMGUI_H
#define POM2_SLOT_MANAGER_IMGUI_H

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace pom2 {

class SlotManager_ImGui
{
public:
    static constexpr int kSlotCount = 8;   // index 1..7 used (0 = LC, host-owned)
    static constexpr int kMaxBays   = 2;   // SmartPort = 2; block cards = 1

    // ── Input snapshot ──────────────────────────────────────────────────
    struct BaySnapshot {
        std::string kindLabel;          // media kind label ("3.5\" 800K"…) or ""
        std::string path;               // mounted image ("" = empty)
        std::string lastError;
        uint32_t    blockCount        = 0;
        bool        loaded            = false;
        bool        writeProtected    = false;
        bool        writeBackEnabled   = false;
        bool        supportsWriteBack  = true;
        bool        supportsTypeSelect = false;   // SmartPort units
        std::string typeKey;            // current type key ("" / "35" / "hdv")
        std::vector<std::pair<std::string, std::string>> typeOptions; // key,label
    };

    struct SlotSnapshot {
        int         slot     = 0;
        bool        occupied = false;   // a card is plugged
        std::string cardKey;            // current card key ("diskii", "hdv"…)
        std::string cardLabel;          // human label
        bool        builtIn  = false;   // profile-forced, type not editable
        std::string builtInBadge;       // badge text (e.g. "built-in serial")
        bool        hasDetailPanel = false;  // a dedicated panel exists
        std::vector<BaySnapshot> bays;  // mountable media bays (may be empty)
    };

    struct PanelSnapshot {
        bool iieMode        = false;    // render the AUX 80-col row
        bool mouseAvailable = false;    // gate the "mouse" dropdown entry
        bool cffaAvailable  = false;    // gate the "cffa" dropdown entry
        std::array<SlotSnapshot, kSlotCount> slots{};
    };

    // ── Output: actions requested this frame ────────────────────────────
    struct BayAction {
        std::string mountPath;          // non-empty = mount this image
        bool        eject            = false;
        bool        writeBackChanged = false;
        bool        writeBackOn      = false;
        bool        typeChanged      = false;
        std::string newType;            // when typeChanged ("" clears the bay)
        bool        boot             = false;   // mount(if path) + boot the slot
    };

    struct Result {
        // Card-assignment edits. When `applyAssignments` is set the host
        // persists `draftCards[1..7]` and restarts the emulator. Built-in
        // slots are kept at their forced key (the panel doesn't edit them).
        bool                          applyAssignments = false;
        std::array<std::string, kSlotCount> draftCards{};
        // Per-slot, per-bay media actions.
        std::array<std::array<BayAction, kMaxBays>, kSlotCount> bays{};
        // Request to open the dedicated detail panel for this slot's card
        // (-1 = none). Host maps slot → the right panel toggle.
        int openDetailForSlot = -1;
    };

    /// Render the panel. `open` is the host's visibility toggle.
    Result render(const char* title, bool& open, const PanelSnapshot& snap);

private:
    // Local draft of the card assignments, edited until Apply. Re-seeded
    // from the live snapshot whenever it diverges (e.g. after a restart).
    std::array<std::string, kSlotCount> draft_{};
    bool draftValid_ = false;

    // Per-(slot,bay) path input buffers — ImGui input boxes need storage
    // that survives across frames.
    std::array<std::array<std::array<char, 512>, kMaxBays>, kSlotCount> pathBufs_{};
    // Track which buffers have been primed from the snapshot path so we
    // don't clobber the user's in-progress typing every frame.
    std::array<std::array<bool, kMaxBays>, kSlotCount> pathPrimed_{};
};

} // namespace pom2

#endif // POM2_SLOT_MANAGER_IMGUI_H

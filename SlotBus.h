// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SlotBus — Apple II expansion bus dispatcher. Owns up to 8 SlotPeripheral
// instances (slots 0-7) and decodes the $C080-$CFFF address space:
//
//   $C080-$C0FF   16-byte device-select per slot (slot N = $C080+N*16).
//   $C100-$C7FF   256-byte slot ROM (slots 1-7). Each access also marks
//                 the corresponding slot as "active" for the shared
//                 expansion ROM window below.
//   $C800-$CFFE   2 KB expansion ROM, routed to whichever slot was most
//                 recently selected by a $CnXX access.
//   $CFFF         Special "disable expansion ROM" switch — read or write
//                 deactivates the active expansion slot until the next
//                 $CnXX touches a slot ROM.
//
// All entry points are called from the CPU thread under
// EmulationController's stateMutex. Plug/unplug/reset must run under the
// same lock so the dispatcher's internal state never races with a CPU
// fetch in flight.

#ifndef POM2_SLOT_BUS_H
#define POM2_SLOT_BUS_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <memory>

class SlotBus
{
public:
    static constexpr int kSlotCount = 8;

    SlotBus() = default;
    ~SlotBus() = default;

    SlotBus(const SlotBus&) = delete;
    SlotBus& operator=(const SlotBus&) = delete;

    /// Insert a card into `slot` (0-7). Replaces and unplugs whatever was
    /// there. Calls `onPlug()` on the new card. Pass `nullptr` to clear
    /// (equivalent to `unplug`).
    void plug(int slot, std::unique_ptr<SlotPeripheral> card);

    /// Remove the card from `slot`, calling `onUnplug()` first. Returns
    /// the unique_ptr so the caller can hold on to it if needed (most
    /// callers just discard it). If the unplugged slot was driving the
    /// expansion ROM, the active expansion slot is cleared.
    std::unique_ptr<SlotPeripheral> unplug(int slot);

    /// Non-owning peek — useful for the UI to display "what's plugged".
    SlotPeripheral* peripheral(int slot) const {
        return (slot >= 0 && slot < kSlotCount) ? slots[slot].get() : nullptr;
    }
    bool isPlugged(int slot) const {
        return slot >= 0 && slot < kSlotCount && slots[slot] != nullptr;
    }

    /// CPU-side dispatch (called by Memory::memRead / memWrite).
    uint8_t deviceSelectRead (uint16_t addr);   // $C080-$C0FF
    void    deviceSelectWrite(uint16_t addr, uint8_t v);
    uint8_t slotRomRead      (uint16_t addr);   // $C100-$C7FF
    uint8_t expansionRomRead (uint16_t addr);   // $C800-$CFFF (CFFF disables)
    void    expansionRomWrite(uint16_t addr, uint8_t v);

    /// $CFFF read/write semantics: clears the active expansion slot.
    /// Exposed so callers (e.g. the Memory dispatcher) can short-circuit
    /// without going through the regular expansionRomRead path.
    void deactivateExpansion() { activeExpansionSlot = -1; }
    int  getActiveExpansionSlot() const { return activeExpansionSlot; }

    /// CPU pacing — forwarded from Memory::advanceCycles().
    void advanceCycles(int cycles);

    /// System soft-switch broadcast — fan-out to every plugged card's
    /// onVideoSoftSwitch(). Used by Memory::softSwitchAccess() for the
    /// switches that aren't in the per-slot device-select range but that
    /// video cards (Le Chat Mauve, Video-7) still need to observe:
    /// $C00C/$C00D (80COL) and $C05E/$C05F (AN3).
    void broadcastVideoSwitch(uint16_t addr);

    /// Apple II RESET line — calls onReset() on every plugged card.
    /// Does NOT touch activeExpansionSlot (matches hardware: RESET
    /// doesn't lift the expansion-ROM enable line).
    void reset();

private:
    std::array<std::unique_ptr<SlotPeripheral>, kSlotCount> slots{};
    /// -1 = no slot driving expansion ROM. Set by slotRomRead, cleared
    /// by $CFFF and by unplug() of the active slot.
    int activeExpansionSlot = -1;
};

#endif // POM2_SLOT_BUS_H

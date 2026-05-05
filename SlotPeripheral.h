// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SlotPeripheral — abstract interface for cards plugged into the Apple II
// expansion bus (slots 0-7). Each card sees three address windows:
//
//   $C0(8+N)X        16-byte device-select per slot
//                    (slot 1 = $C090-$C09F, ..., slot 7 = $C0F0-$C0FF.
//                     slot 0 = $C080-$C08F = language card slot.)
//   $C(N)XX          256-byte slot ROM (slots 1-7 only — slot 0 has no
//                    slot ROM space; $C000-$C0FF is the I/O page).
//   $C800-$CFFF      2 KB expansion ROM, shared between all slots —
//                    one slot active at a time, switched by the SlotBus
//                    based on which slot's ROM was last touched.
//
// All callbacks are invoked from the CPU thread under EmulationController's
// stateMutex; cards don't need to lock state for those. Lifecycle hooks
// (onPlug / onUnplug / onReset) are also called under that lock so card
// initialisation can mutate the bus freely.
//
// Default implementations match an empty/inactive socket (read = $FF "open
// bus", writes silently dropped). Override only the methods your card
// actually implements.

#ifndef POM2_SLOT_PERIPHERAL_H
#define POM2_SLOT_PERIPHERAL_H

#include <cstdint>
#include <string_view>

class SlotPeripheral
{
public:
    virtual ~SlotPeripheral() = default;

    /// Short human-readable name (e.g. "Disk II", "Language Card", "80col").
    /// Surfaced in the Hardware menu / status bar.
    virtual std::string_view name() const = 0;

    /// $C0(8+N)X — 16-byte device-select range. `low4` is the low nibble
    /// of the address (0..15). Reads and writes are dispatched separately
    /// so a card can implement asymmetric semantics (e.g. Disk II's $C0nE
    /// is "read mode" on read, "set Q6" on write — both legal accesses).
    virtual uint8_t deviceSelectRead (uint8_t /*low4*/)              { return 0xFF; }
    virtual void    deviceSelectWrite(uint8_t /*low4*/, uint8_t /*v*/) {}

    /// $C(N)XX — 256-byte slot ROM. `low8` is the low byte of the address
    /// (0..255). Slot ROM is read-only on real hardware; writes never
    /// reach this method (the SlotBus drops them).
    virtual uint8_t slotRomRead(uint8_t /*low8*/) { return 0xFF; }

    /// $C800-$CFFF — 2 KB expansion ROM. `offset` is the byte offset into
    /// the 2 KB window (0..0x7FE; 0x7FF is intercepted by SlotBus as the
    /// "$CFFF disable" switch and never reaches the card). Expansion ROM
    /// is conventionally read-only, but we forward writes anyway so cards
    /// that hide soft switches in this window (rare) keep working.
    virtual uint8_t expansionRomRead (uint16_t /*offset*/) { return 0xFF; }
    virtual void    expansionRomWrite(uint16_t /*offset*/, uint8_t /*v*/) {}

    /// Lifecycle. `onPlug` / `onUnplug` flank a card insertion; `onReset`
    /// fires on Apple II hardware reset (Ctrl-Reset).
    virtual void onPlug()   {}
    virtual void onUnplug() {}
    virtual void onReset()  {}

    /// Cycle pacing — for cycle-driven peripherals (Disk II's stepper, a
    /// future serial card's UART, …). Forwarded by SlotBus from
    /// Memory::advanceCycles().
    virtual void advanceCycles(int /*cycles*/) {}
};

#endif // POM2_SLOT_PERIPHERAL_H

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

class SlotBus;

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
    /// (0..255). On real hardware most cards leave this read-only, but a
    /// handful (notably the Sweet Microsystems Mockingboard, which decodes
    /// its 6522 VIAs at $Cn00 / $Cn80 instead of in the 16-byte device-
    /// select range) treat $CnXX as memory-mapped I/O. The SlotBus
    /// forwards both reads and writes; cards that don't override
    /// `slotRomWrite` get the default no-op (matching ROM behaviour).
    virtual uint8_t slotRomRead (uint8_t /*low8*/)              { return 0xFF; }
    virtual void    slotRomWrite(uint8_t /*low8*/, uint8_t /*v*/) {}

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

    /// System soft-switch broadcast — fires for switches outside the
    /// per-slot device-select range that some cards still need to observe
    /// (Le Chat Mauve / Video-7 sniff $C00C/$C00D 80COL and $C05E/$C05F
    /// AN3 to clock their 2-bit FIFO mode register). Forwarded by
    /// Memory::softSwitchAccess() via SlotBus::broadcastVideoSwitch().
    virtual void onVideoSoftSwitch(uint16_t /*addr*/) {}

    /// Slot number assigned by the bus at plug-time (1..7), or -1 before
    /// `SlotBus::plug()` adopts the card. Concrete cards may still carry
    /// their own constructor-time slot field for ROM addressing reasons
    /// (the SSC bakes slot into its PR# trampolines), but `busSlot()` is
    /// the authoritative source once attached.
    int busSlot() const { return busSlot_; }

    /// Whether this slot's contribution to the wire-OR'd CPU IRQ line is
    /// currently asserted. Mirrors what `assertIrq()` last published to
    /// the bus — useful for debug panels and tests.
    bool slotIrqAsserted() const { return irqAsserted_; }

protected:
    /// Assert (true) or release (false) this slot's contribution to the
    /// CPU's wire-OR'd IRQ line. Idempotent — repeated true→true or
    /// false→false calls are no-ops, so cards don't need their own edge
    /// tracking. Safe to call before plug (becomes a no-op until SlotBus
    /// attaches the card). `SlotBus::plug()` and `SlotBus::unplug()`
    /// auto-release any still-asserted bit before letting the card go,
    /// so cards rarely need to clear in `onUnplug()` themselves.
    void assertIrq(bool asserted);

private:
    friend class SlotBus;
    /// Called by SlotBus::plug() right after onPlug(). Wires the card to
    /// its bus + slot number so `assertIrq()` can fan out.
    void attachToBus(SlotBus* bus, int slot);
    /// Called by SlotBus::plug()/unplug() right before onUnplug(). Drops
    /// any pending IRQ contribution and clears the bus pointer so a
    /// post-unplug stray assertIrq() is a no-op.
    void detachFromBus();

    SlotBus* bus_       = nullptr;
    int      busSlot_   = -1;
    bool     irqAsserted_ = false;
};

#endif // POM2_SLOT_PERIPHERAL_H

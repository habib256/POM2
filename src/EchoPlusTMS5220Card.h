// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// EchoPlusTMS5220Card — Street Electronics ECHO+ as actually shipped.
//
// Reverse-engineered chipset (markadev/AppleII-RevEng/Street-Electronics
// -Corp-ECHO+, index.md): two AY-3-8913 PSGs + one TMS5220 (LPC speech).
// Distinct from POM2's `EchoPlusCard` (which models the SSI263-based
// Cricket-class card historically mis-labelled as "Echo+").
//
// Status — v1 scaffold
// --------------------
// The full TMS5220 LPC decoder is a 600+ LOC port (chirp ROM, K-parameter
// interpolation, energy / pitch tables) that doesn't fit a single-commit
// slice. AY-3-8913 audio synth is also stubbed — POM2 already has the
// register bank (`Ay3_8910.h`); the synthesis core lives in
// MockingboardCard / PhasorCard and would need to be lifted to a shared
// helper before this card can use it.
//
// What this card provides today:
//   * SlotPeripheral registration so the user can pick "Echo+ (TMS5220)"
//     in Slot Configuration. The card occupies its slot, soft-resets
//     cleanly, and never crashes the bus.
//   * Stub register decode at the documented $Cs00-$Cs0F window so
//     software that probes for the card finds something coherent (vs
//     open-bus everywhere). Writes are accepted and silently dropped;
//     reads return a status byte with the TMS5220 BL ("buffer low") flag
//     pinned to 0 so naive probes see "speech in progress, please poll"
//     and back off cleanly.
//
// Address map (slot s)
// --------------------
// Pin to markadev's schematic when the LPC core lands. Provisional map
// (TODO refine against the schematic):
//
//   $Cs00       TMS5220 status / data (rd = status, wr = command/data byte)
//   $Cs01       TMS5220 stop / reset
//   $Cs04..05   AY-3-8913 #1 (address latch / data)
//   $Cs06..07   AY-3-8913 #2 (address latch / data)
//   $Cs08..FF   open bus
//
// No 6522 in the real Echo+ — the AYs are wired direct to the slot bus
// (BC1 / BDIR strapped via address decode), unlike Mockingboard.

#ifndef POM2_ECHO_PLUS_TMS5220_CARD_H
#define POM2_ECHO_PLUS_TMS5220_CARD_H

#include "Ay3_8910.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string_view>

class EchoPlusTMS5220Card : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 2;     // documented Echo+ slot

    explicit EchoPlusTMS5220Card(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Echo+ (TMS5220 + 2×AY)"; }
    uint8_t slotRomRead  (uint8_t low8) override;
    void    slotRomWrite (uint8_t low8, uint8_t v) override;
    void    onReset() override;

    // ─── Test / UI hooks ─────────────────────────────────────────────────
    struct Snap {
        uint8_t tmsStatus;       // last status byte returned
        uint8_t tmsLastWrite;    // last byte written to TMS5220 data port
        uint8_t ayRegs[2][16];   // two AY-3-8913s, 16 registers each
    };
    Snap snapshot() const;

private:
    int slot_;
    mutable std::mutex mtx_;

    // TMS5220 scaffold — status byte exposed at $Cs00 read with the BL
    // (buffer low) flag held low so an idle card looks "not ready to
    // accept the next FIFO byte yet", letting probe loops poll forever
    // without our card claiming it has anything to say.
    uint8_t tmsStatus_    = 0x00;
    uint8_t tmsLastWrite_ = 0x00;

    // Two AY-3-8913 register banks — synthesis core deferred to the
    // shared Mockingboard/Phasor audio thread refactor.
    pom2::Ay3_8910 ay_[2];
    uint8_t        aySelected_[2] = { 0, 0 };
};

#endif // POM2_ECHO_PLUS_TMS5220_CARD_H

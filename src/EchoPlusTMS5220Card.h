// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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
//     status reads return the real chip's idle byte: TS=0, BL=1, BE=1
//     = $60 (MAME `tms5220.cpp:894-907` status_read; `:1771` reset sets
//     buffer_empty = buffer_low = true). Probe loops therefore see
//     "not talking, FIFO ready for data" and proceed — a driver gating
//     its FIFO writes on BL=1 would hang forever against the earlier
//     BE-only ($20) scaffold value.
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
#include <vector>

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

    /// Rewind / snapshot state. Scaffold or not, every byte this card owns is
    /// GUEST-VISIBLE: `$Cs00` reads the TMS status and `$Cs04-$Cs07` read back
    /// the selected AY register, so a rewind that left them at the live values
    /// would drop a restored driver onto registers from a timeline it never
    /// executed. (Serialising nothing was the 2026-07-29 workflow hunt's
    /// finding about five other cards; this one arrived later and inherited
    /// the same gap — see bug hunt 8.) Foreign blobs are ignored via the magic.
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

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

    // TMS5220 scaffold — status byte exposed at $Cs00 read. Idle/reset
    // value is $60 (BL|BE set, TS clear), matching MAME tms5220.cpp's
    // status_read packing + reset state (see onReset()). onReset()
    // (re)initialises it before any read can observe this default.
    uint8_t tmsStatus_    = 0x60;
    uint8_t tmsLastWrite_ = 0x00;

    // Two AY-3-8913 register banks — synthesis core deferred to the
    // shared Mockingboard/Phasor audio thread refactor.
    pom2::Ay3_8910 ay_[2];
    uint8_t        aySelected_[2] = { 0, 0 };
};

#endif // POM2_ECHO_PLUS_TMS5220_CARD_H

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

// Vapor-lock reality check.
//
// "Vapor lock" = a demo writes a marker byte into video RAM, then spins reading
// an undriven $C0xx (the floating bus = the byte the video scanner is fetching
// *this cycle*) until it reads the marker back — at which instant it knows where
// the beam is, to the cycle, and starts beam-racing. For this to work POM2's
// Memory::floatingBus() must (a) track the scanner per cycle so a marker is read
// for a catchable run of cycles, (b) let a real 6502 poll loop actually lock,
// and (c) sweep the *active video standard's* frame (262 NTSC / 312 PAL) so the
// per-frame lock is stable — French Touch / DIX are PAL.

#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {
constexpr uint8_t MARKER = 0x7E;
const long kNtscFrame = (long)POM2_TIMING_NTSC.cyclesPerScanline *
                        POM2_TIMING_NTSC.scanlinesPerFrame;     // 65*262 = 17030
const long kPalFrame  = (long)POM2_TIMING_PAL.cyclesPerScanline *
                        POM2_TIMING_PAL.scanlinesPerFrame;      // 65*312 = 20280

void setupMarkerRow(Memory& mem) {
    for (uint16_t a = 0x0400; a < 0x0800; ++a) mem.memWrite(a, 0xA0);   // spaces
    for (uint16_t a = 0x0400; a <= 0x0427; ++a) mem.memWrite(a, MARKER);  // row 0
}
uint8_t fbAt(Memory& mem, long c) {
    mem.setCycleCounter(static_cast<uint64_t>(c));
    return mem.peekFloatingBus();
}
}  // namespace

int main()
{
    // ── (a) The scanner sweeps the marker, in catchable runs. ─────────────
    Memory mem;                 // II+ default, text page 1
    mem.resetSoftSwitches();
    setupMarkerRow(mem);

    int hits = 0, curRun = 0, maxRun = 0;
    for (long c = 0; c < kNtscFrame; ++c) {
        if (fbAt(mem, c) == MARKER) { ++hits; ++curRun; maxRun = std::max(maxRun, curRun); }
        else curRun = 0;
    }
    std::printf("  NTSC: marker read %d cyc/frame, longest contiguous run %d\n", hits, maxRun);
    assert(hits > 0 && "floating bus never reads the marker — scanner broken");
    assert(maxRun >= 16 && "no run long enough for a poll loop to catch the marker");

    // ── (b) A real 6502 $C040 poll loop LOCKS. ────────────────────────────
    M6502 cpu(&mem);
    cpu.hardReset();
    mem.resetSoftSwitches();
    setupMarkerRow(mem);
    // $C058 (annunciator AN0) is an undriven read → returns the floating bus,
    // with no display side effect on a II+ (unlike $C050-$C057). $C040 swallows
    // the access (game-port STRB), so it can't be used to sample the bus.
    const uint8_t prog[] = {
        0xAD, 0x58, 0xC0,   // loop: LDA $C058  (floating bus)
        0xC9, MARKER,       //       CMP #$7E
        0xD0, 0xF9,         //       BNE loop
        0xA9, 0x01,         //       LDA #$01
        0x85, 0x00,         //       STA $00     ; "locked"
        0x4C, 0x0B, 0x03,   //       JMP * (park)
    };
    for (size_t i = 0; i < sizeof(prog); ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), prog[i]);
    mem.memWrite(0x0000, 0x00);
    mem.setCycleCounter(0);
    cpu.setProgramCounter(0x0300);
    cpu.run(kNtscFrame * 2);                          // up to two frames
    assert(mem.memRead(0x0000) == 0x01 &&
           "vapor-lock poll loop never locked within two frames");
    std::printf("  NTSC: real 6502 $C058 poll loop LOCKED on the marker\n");

    // ── (c) Geometry follows the video standard (the PAL fix). ────────────
    // Fill the scanner's reachable RAM with a per-address hash so the floating
    // bus byte reflects the scanner ADDRESS, then verify peekFloatingBus is
    // periodic with the standard's frame — and NOT the other standard's.
    for (uint32_t a = 0; a < 0xC000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>((a * 181u) & 0xFF));
    auto periodicWith = [&](long period) {
        for (long c = 0; c < 4000; ++c)
            if (fbAt(mem, c) != fbAt(mem, c + period)) return false;
        return true;
    };

    mem.setVideoStandard(VideoStandard::NTSC);
    assert(periodicWith(kNtscFrame) && "NTSC: floating bus not periodic with 262-line frame");

    mem.setVideoStandard(VideoStandard::PAL);
    assert(periodicWith(kPalFrame) &&
           "PAL: floating bus not periodic with 312-line frame");
    assert(!periodicWith(kNtscFrame) &&
           "PAL: floating bus STILL repeats every 262 lines — not PAL-aware "
           "(this is the bug that drifts DIX's per-frame vapor lock)");
    std::printf("  NTSC frame=%ld cyc (262 lines), PAL frame=%ld cyc (312 lines): "
                "geometry follows the standard\n", kNtscFrame, kPalFrame);

    // ── (d) DROL cut-scene: a $C050 READ flips the mode AND returns the
    // floating bus. Drol.dsk (offsets 0x14359 / 0x143d5 / 0x14be0) syncs its
    // cut-scenes with
    //     LDX #$02 / l: LDA $C050 / CMP #$80 / BNE l / DEX / BPL l
    // — three consecutive scanner reads of $80 from a display soft switch.
    // POM2 used to return a hard 0 for $C050-$C057 reads → the loop span
    // forever (same hang LinApple had; AppleWin fixed it in 1.13.0 with the
    // floating bus). The HGR page is filled with $80 (black-with-palette-bit,
    // DROL's cleared background), so once the loop's own $C050 access has
    // dropped TEXT the scanner serves $80 and the lock must take.
    mem.setVideoStandard(VideoStandard::NTSC);
    mem.resetSoftSwitches();
    (void)mem.memRead(0xC051);                                  // TEXT on
    (void)mem.memRead(0xC057);                                  // HIRES armed
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), 0x80);
    const uint8_t cut[] = {
        0xA2, 0x02,         //       LDX #$02
        0xAD, 0x50, 0xC0,   // l:    LDA $C050   (gfx on + floating bus)
        0xC9, 0x80,         //       CMP #$80
        0xD0, 0xF9,         //       BNE l
        0xCA,               //       DEX
        0x10, 0xF6,         //       BPL l
        0xA9, 0x01,         //       LDA #$01
        0x85, 0x00,         //       STA $00     ; "locked"
        0x4C, 0x10, 0x03,   //       JMP * (park)
    };
    for (size_t i = 0; i < sizeof(cut); ++i)
        mem.memWrite(static_cast<uint16_t>(0x0300 + i), cut[i]);
    mem.memWrite(0x0000, 0x00);
    mem.setCycleCounter(0);
    cpu.setProgramCounter(0x0300);
    cpu.run(kNtscFrame * 2);
    assert(mem.memRead(0x0000) == 0x01 &&
           "DROL cut-scene loop never locked — $C050 read must return the "
           "floating bus, not 0");
    assert(!mem.getDisplayState().textMode &&
           "$C050 read lost its display side effect");
    std::printf("  DROL cut-scene $C050 poll loop LOCKED (read = floating bus"
                " + mode flip)\n");

    std::printf("OK vapor_lock\n");
    return 0;
}

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

// SlotRomBuilder — the guard itself.
//
// Every card that publishes `romLayoutError()` is only as trustworthy as this
// class, and each of those tests can only assert the flag is CLEAR. Something
// has to assert it can be SET, or "no card reports a layout error" would also
// be true of a guard wired to `return false`. That is this file.
//
// The bug being guarded is in SlotRom.h: SmartPortCard's write-block routine
// silently overwrote the ProDOS STATUS routine the dispatch table jumped to,
// so STATUS was dead code for weeks and answered $27 "I/O error" from a
// perfectly healthy bay. Both directions are caught here, because both are
// real: a region that GROWS eats its neighbour, and a region that SHRINKS
// breaks the hand-computed branch displacements that point past it.

#include "SlotRom.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void expect(bool cond, const std::string& what)
{
    if (!cond) { std::printf("FAIL: %s\n", what.c_str()); ++failures; }
}

using Page = std::array<uint8_t, pom2::kSlotRomBytes>;

} // namespace

int main()
{
    // ── A region that fits is not an error, and writes exactly its bytes ──
    {
        Page rom{};
        rom.fill(0xEA);
        pom2::SlotRomBuilder b(rom);
        b.region(0x20, 0x30).emit({ 0xA9, 0x01, 0x60 });
        expect(!b.layoutError(), "a region inside its budget must not flag");
        expect(b.pc() == 0x23,   "pc must advance by the bytes written");
        expect(b.room() == 0x0D, "room() must report the slack left");
        expect(rom[0x20] == 0xA9 && rom[0x21] == 0x01 && rom[0x22] == 0x60,
               "the bytes must land at the region's start");
        expect(rom[0x23] == 0xEA, "nothing may be written past the last byte");
    }

    // ── Exactly filling a region is legal — several real layouts do it ────
    // FujiNetCard's boot routine ends on the very byte before its successor,
    // and ProDOSHardDiskCard's write routine ends one byte below STATUS.
    // Neither may be reported as an overflow, or the guard cries wolf on the
    // layouts it is meant to protect.
    {
        Page rom{};
        pom2::SlotRomBuilder b(rom);
        b.region(0x00, 0x04).emit({ 1, 2, 3, 4 });
        expect(!b.layoutError(), "an exactly-full region must not flag");
        expect(b.pc() == 0x04,   "pc must sit on the limit after an exact fit");
        expect(b.room() == 0,    "an exactly-full region has no room left");
    }

    // ── One byte too many: flagged, and the neighbour is NOT touched ──────
    // This is the SmartPort bug. The old `rom[pc++]` wrote the byte anyway.
    {
        Page rom{};
        rom.fill(0xEA);
        rom[0x04] = 0x4C;                       // the neighbour's first byte
        pom2::SlotRomBuilder b(rom);
        b.region(0x00, 0x04).emit({ 1, 2, 3, 4, 5 });
        expect(b.overflowed(),  "a region one byte over budget must flag");
        expect(b.layoutError(), "overflow must surface through layoutError()");
        expect(rom[0x04] == 0x4C,
               "an over-budget region must not overwrite its neighbour");
    }

    // ── The flag is sticky: a later well-behaved region cannot clear it ───
    {
        Page rom{};
        pom2::SlotRomBuilder b(rom);
        b.region(0x00, 0x02).emit({ 1, 2, 3 });
        b.region(0x10, 0x20).emit({ 1, 2 });
        expect(b.layoutError(), "the flag must survive a later good region");
    }

    // ── A region that ends in the wrong place is flagged too ─────────────
    // Nothing overflows here: the region SHRANK. Every one of these ROMs is
    // full of `BEQ +55`-style displacements computed by hand against where a
    // routine was expected to end, and a shrink breaks them just as
    // thoroughly as a grow — silently, and without changing a single byte
    // that a hexdump comparison would notice.
    {
        Page rom{};
        pom2::SlotRomBuilder b(rom);
        b.region(0x50, 0x70).emit({ 1, 2, 3 }).expectEnd(0x66);
        expect(!b.overflowed(), "a short region has not overflowed");
        expect(b.misaligned(),  "a region ending early must be flagged");
        expect(b.layoutError(), "misalignment must surface through layoutError()");
    }
    {
        Page rom{};
        pom2::SlotRomBuilder b(rom);
        b.region(0x50, 0x70).emit({ 1, 2, 3 }).expectEnd(0x53);
        expect(!b.layoutError(), "a region ending exactly where declared is fine");
    }

    // ── The page end is a hard stop, because it used to WRAP ─────────────
    // `pc` was a uint8_t: a region running off $CnFF continued at $Cn00 and
    // rewrote the card's own signature bytes. A limit of 256 is not special-
    // cased anywhere, so the class has to enforce the page itself.
    {
        Page rom{};
        rom.fill(0xEA);
        pom2::SlotRomBuilder b(rom);
        b.region(0xFE, 0x120).emit({ 1, 2, 3, 4 });
        expect(b.overflowed(), "running off the end of the page must flag");
        expect(rom[0x00] == 0xEA, "an overrun must not wrap onto $Cn00");
        expect(rom[0xFE] == 1 && rom[0xFF] == 2,
               "the bytes that did fit must still be written");
    }

    // ── slotRomRel: the displacement the hand-assembled ROMs branch on ───
    // Forwards, backwards, and the two-byte bias (the operand follows the
    // opcode, so the CPU adds to `at + 2`).
    {
        expect(pom2::slotRomRel(0x1E, 0x30) == 0x10, "forward branch");
        expect(pom2::slotRomRel(0x57, 0x50) == 0xF7, "backward branch");
        expect(pom2::slotRomRel(0x10, 0x12) == 0x00, "branch to the next insn");
    }

    if (failures == 0) std::printf("slot_rom_builder: OK\n");
    return failures == 0 ? 0 : 1;
}

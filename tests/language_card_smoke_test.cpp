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

// Language Card smoke test — pins the 16 KB LC state machine ($C080-$C08F):
//   * ROM is visible by default
//   * $C080/$C088 read-enable RAM bank 2/1 in write-protected mode
//   * $C083/$C08B require the two-access prewrite latch before writes stick
//   * $C081 can write RAM while ROM stays visible
//   * $E000-$FFFF is shared between the two $D000-$DFFF banks
//
// Runs in IIe mode: the $C011 (RDBNK2) / $C012 (RDLCRAM) status registers
// used here to observe the state machine exist only on the IIe MMU. On a
// real II/II+ the whole $C010-$C01F page is the keyboard-strobe-clear
// mirror (MAME `apple2.cpp:548` maps $C010 with `.mirror(0xf)`) — POM2
// previously answered $C011/$C012 with IIe-style LC status on II+ too,
// which this test used to pin. The $C08x state machine itself is identical
// on both machine classes (same MAME `lc_update` port).

#include "Memory.h"

#include <cassert>
#include <cstdio>

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    const uint8_t romBytes[] = { 0xD0, 0xE0 };
    assert(mem.loadRomBytes(&romBytes[0], 1, 0xD000));
    assert(mem.loadRomBytes(&romBytes[1], 1, 0xE000));

    // Default after construction/reset: ROM visible, LC writes protected.
    assert(mem.memRead(0xD000) == 0xD0);
    assert(mem.memRead(0xE000) == 0xE0);
    assert(mem.memRead(0xC012) == 0x00);

    // One $C083 access selects RAM bank 2 for reads but does not enable writes.
    (void)mem.memRead(0xC083);
    mem.memWrite(0xD000, 0x22);
    assert(mem.memRead(0xD000) == 0x00);

    // Second consecutive write-enable access arms writes.
    (void)mem.memRead(0xC083);
    mem.memWrite(0xD000, 0x22);
    assert(mem.memRead(0xD000) == 0x22);
    assert(mem.memRead(0xC011) == 0x80); // bank 2 selected
    assert(mem.memRead(0xC012) == 0x80); // RAM visible

    // Bank 1 is a separate 4 KB window at $D000-$DFFF.
    (void)mem.memRead(0xC08B);
    (void)mem.memRead(0xC08B);
    mem.memWrite(0xD000, 0x11);
    assert(mem.memRead(0xD000) == 0x11);
    assert(mem.memRead(0xC011) == 0x00); // bank 1 selected

    (void)mem.memRead(0xC083);
    assert(mem.memRead(0xD000) == 0x22);

    // $E000-$FFFF is shared high LC RAM, not duplicated per bank.
    mem.memWrite(0xE000, 0xEE);
    assert(mem.memRead(0xE000) == 0xEE);
    (void)mem.memRead(0xC08B);
    assert(mem.memRead(0xE000) == 0xEE);

    // $C081 write-only mode: reads come from ROM, writes still target LC RAM.
    (void)mem.memRead(0xC081);
    (void)mem.memRead(0xC081);
    assert(mem.memRead(0xD000) == 0xD0);
    mem.memWrite(0xD000, 0x44);
    (void)mem.memRead(0xC080);
    assert(mem.memRead(0xD000) == 0x44);

    // ROM-only switches protect the LC again.
    (void)mem.memRead(0xC082);
    assert(mem.memRead(0xD000) == 0xD0);
    mem.memWrite(0xD000, 0x99);
    (void)mem.memRead(0xC080);
    assert(mem.memRead(0xD000) == 0x44);

    // ── II+ machine class: same $C08x state machine, observed through
    // ROM/RAM visibility only — $C011/$C012 are the keyboard-strobe
    // mirror on II/II+, so the IIe status registers used above don't
    // exist there. Keeps the II+ LC path pinned end-to-end (the IIe-mode
    // switch above would otherwise leave it covered by zero tests).
    {
        Memory m2;   // default = II/II+ mode
        const uint8_t romBytes2[] = { 0xD0, 0xE0 };
        assert(m2.loadRomBytes(&romBytes2[0], 1, 0xD000));
        assert(m2.loadRomBytes(&romBytes2[1], 1, 0xE000));
        assert(m2.memRead(0xD000) == 0xD0);   // ROM by default
        (void)m2.memRead(0xC083);             // 1st: read-RAM bank 2, no write
        m2.memWrite(0xD000, 0x22);
        assert(m2.memRead(0xD000) == 0x00);   // prewrite not armed
        (void)m2.memRead(0xC083);             // 2nd consecutive: writes armed
        m2.memWrite(0xD000, 0x22);
        assert(m2.memRead(0xD000) == 0x22);
        (void)m2.memRead(0xC08B);             // bank 1, double access
        (void)m2.memRead(0xC08B);
        m2.memWrite(0xD000, 0x11);
        assert(m2.memRead(0xD000) == 0x11);   // bank 1 is a separate window
        (void)m2.memRead(0xC083);
        assert(m2.memRead(0xD000) == 0x22);   // bank 2 contents retained
        (void)m2.memRead(0xC082);
        assert(m2.memRead(0xD000) == 0xD0);   // ROM-only protects again
        std::printf("Language Card II+ machine class: OK\n");
    }

    std::printf("Language Card smoke: OK\n");
    return 0;
}

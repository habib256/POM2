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

    std::printf("Language Card smoke: OK\n");
    return 0;
}

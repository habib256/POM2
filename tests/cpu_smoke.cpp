// POM2 — quick CPU + Memory + soft-switch smoke test.
// Run from POM2 root:
//   g++ -std=c++17 -O0 -I. tests/cpu_smoke.cpp M6502.cpp Memory.cpp -o /tmp/pom2_smoke && /tmp/pom2_smoke

#include "M6502.h"
#include "Memory.h"
#include <cassert>
#include <cstdio>

int main()
{
    Memory mem;
    M6502  cpu(&mem);

    // Stuff a tiny program at $0300 that writes 'A' to the top-left of the
    // text screen, toggles the speaker once, then infinite-loops:
    //   $0300 LDA #$C1     A9 C1     ; 'A' in the firmware's screen encoding
    //   $0302 STA $0400    8D 00 04
    //   $0305 LDA $C030    AD 30 C0  ; speaker click
    //   $0308 JMP $0308    4C 08 03
    auto* m = mem.dataMutable();
    m[0x300] = 0xA9; m[0x301] = 0xC1;
    m[0x302] = 0x8D; m[0x303] = 0x00; m[0x304] = 0x04;
    m[0x305] = 0xAD; m[0x306] = 0x30; m[0x307] = 0xC0;
    m[0x308] = 0x4C; m[0x309] = 0x08; m[0x30A] = 0x03;

    // Force PC straight at $0300 (skip the boot-time reset vector).
    cpu.setProgramCounter(0x0300);

    int totalCycles = 0;
    for (int i = 0; i < 100; ++i) {
        cpu.step();
        totalCycles += 1;     // step() doesn't expose its own counter
    }

    const uint8_t cellByte = m[0x400];
    const auto    speaker  = mem.getSpeakerToggleCount();

    std::printf("text[$0400] = $%02X (expected $C1)\n", cellByte);
    std::printf("speaker toggles = %llu (expected >= 1)\n",
                (unsigned long long)speaker);
    std::printf("PC after %d steps = $%04X\n", 100, cpu.getProgramCounter());

    assert(cellByte == 0xC1 && "text screen write failed");
    assert(speaker >= 1     && "speaker click missed");
    assert(cpu.getProgramCounter() == 0x0308 && "should be parked in JMP loop");

    std::printf("OK\n");
    return 0;
}

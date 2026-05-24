// POM2 — boot the Apple II+ ROM headless and dump the text screen.
// Build:
//   g++ -std=c++17 -O2 -I. tests/rom_boot.cpp M6502.cpp Memory.cpp -o /tmp/pom2_rom_boot
// Run from POM2 root (so roms/apple2.rom resolves):
//   /tmp/pom2_rom_boot

#include "M6502.h"
#include "Memory.h"
#include <cstdio>
#include <cstdlib>

// Apple II text/lo-res row interleave (mirrors Apple2Display::textRowAddress).
static uint16_t textRowAddr(int y) {
    return static_cast<uint16_t>(0x0400 + 0x80 * (y & 7) + 0x28 * (y >> 3));
}

// Translate a screen byte to a printable ASCII char. The firmware sets the
// high bit on bytes it stores to text memory; we strip it for the dump.
static char screenByteToAscii(uint8_t b) {
    uint8_t a = b & 0x7F;
    return (a >= 0x20 && a < 0x7F) ? static_cast<char>(a) : '.';
}

int main(int argc, char* argv[])
{
    Memory mem;
    M6502  cpu(&mem);

    if (!mem.loadAppleIIRom("roms/apple2.rom")) {
        std::fprintf(stderr, "ROM load failed: %s\n", mem.getLastError().c_str());
        return 1;
    }

    cpu.hardReset();

    // Run plenty of cycles for the autostart Monitor to clear screen, print
    // the "APPLE ][" header, and drop into Applesoft. ~400 000 cycles ≈
    // 0.4 seconds emulated — well past the boot delay.
    long maxCycles = (argc > 1) ? std::atol(argv[1]) : 1'000'000;
    long ran = 0;
    while (ran < maxCycles) {
        ran += cpu.run(20000);   // run() returns actual cycles
        mem.advanceCycles(20000);
    }

    std::printf("Booted %ld cycles. PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X\n",
                ran, cpu.getProgramCounter(),
                cpu.getAccumulator(), cpu.getXRegister(),
                cpu.getYRegister(), cpu.getStackPointer());

    auto state = mem.getDisplayState();
    std::printf("Display: text=%d mixed=%d page2=%d hires=%d\n",
                state.textMode, state.mixedMode, state.page2, state.hiRes);

    // Dump the 24×40 text screen (page 1) — what the user would see on the
    // monitor right now.
    std::printf("\n--- text screen (page 1) ---\n");
    const uint8_t* ram = mem.data();
    for (int y = 0; y < 24; ++y) {
        const uint16_t base = textRowAddr(y);
        char line[41];
        for (int x = 0; x < 40; ++x)
            line[x] = screenByteToAscii(ram[base + x]);
        line[40] = '\0';
        std::printf("%2d |%s|\n", y, line);
    }

    return 0;
}

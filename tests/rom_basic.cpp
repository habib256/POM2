// POM2 — boot ROM, type a tiny Applesoft program, RUN it, dump the screen.
//   g++ -std=c++17 -O2 -I. tests/rom_basic.cpp M6502.cpp Memory.cpp -o /tmp/pom2_basic
//   ./build/POM2 must NOT be running concurrently (this is headless).

#include "M6502.h"
#include "Memory.h"
#include <cstdio>
#include <string>
#include <thread>

static uint16_t textRowAddr(int y) {
    return static_cast<uint16_t>(0x0400 + 0x80 * (y & 7) + 0x28 * (y >> 3));
}
static char glyph(uint8_t b) {
    uint8_t a = b & 0x7F;
    return (a >= 0x20 && a < 0x7F) ? static_cast<char>(a) : '.';
}

static void runFor(M6502& cpu, Memory& mem, long cycles) {
    long ran = 0;
    while (ran < cycles) {
        ran += cpu.run(10000);
        mem.advanceCycles(10000);
    }
}

// We have to wait between keys for the ROM to pick the byte up — feed one
// key, run a chunk of cycles, repeat.
static void typeKey(M6502& cpu, Memory& mem, char c) {
    mem.queueKey(static_cast<uint8_t>(c));
    runFor(cpu, mem, 30000);
}

static void typeString(M6502& cpu, Memory& mem, const std::string& s) {
    for (char c : s) typeKey(cpu, mem, c);
}

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    if (!mem.loadAppleIIRom("roms/apple2.rom")) {
        std::fprintf(stderr, "ROM load failed: %s\n", mem.getLastError().c_str());
        return 1;
    }
    cpu.hardReset();

    // Cold boot — give Applesoft time to print the prompt.
    runFor(cpu, mem, 600000);

    // Tiny Applesoft demo: print HELLO POM2 in a loop a few times.
    typeString(cpu, mem, "10 FOR I=1 TO 3\r");
    typeString(cpu, mem, "20 PRINT \"HELLO POM2\"\r");
    typeString(cpu, mem, "30 NEXT I\r");
    typeString(cpu, mem, "RUN\r");

    // Let the program execute and idle for a moment.
    runFor(cpu, mem, 800000);

    std::printf("\n--- text screen after RUN ---\n");
    const uint8_t* ram = mem.data();
    for (int y = 0; y < 24; ++y) {
        const uint16_t base = textRowAddr(y);
        char line[41];
        for (int x = 0; x < 40; ++x) line[x] = glyph(ram[base + x]);
        line[40] = '\0';
        std::printf("%2d |%s|\n", y, line);
    }
    std::printf("\nFinal CPU: PC=$%04X A=$%02X X=$%02X Y=$%02X SP=$%02X\n",
                cpu.getProgramCounter(), cpu.getAccumulator(),
                cpu.getXRegister(), cpu.getYRegister(),
                cpu.getStackPointer());
    return 0;
}

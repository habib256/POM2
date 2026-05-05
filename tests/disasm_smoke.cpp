// POM2 — disassemble a few instructions from the Apple II Monitor ROM.
//   g++ -std=c++17 -O2 -I. tests/disasm_smoke.cpp Memory.cpp Disassembler6502.cpp -o /tmp/pom2_disasm

#include "Disassembler6502.h"
#include "Memory.h"
#include <cstdio>

int main()
{
    Memory mem;
    if (!mem.loadAppleIIRom("roms/apple2.rom")) {
        std::fprintf(stderr, "ROM load failed: %s\n", mem.getLastError().c_str());
        return 1;
    }

    // The 6502 reset vector lives at $FFFC/D — read it and disassemble the
    // first 12 instructions of whatever it points to. That entry point is
    // the cold-boot routine; we only check that the disassembler produces
    // plausible mnemonics rather than checking specific bytes (ROM hashes
    // are part of their own test).
    const uint8_t* m = mem.data();
    const uint16_t resetVec = m[0xFFFC] | (m[0xFFFD] << 8);
    std::printf("Reset vector $FFFC = $%04X\n", resetVec);

    uint16_t pc = resetVec;
    for (int i = 0; i < 12; ++i) {
        int len = 1;
        std::string mnem = pom2::disassemble6502(m, pc, len);
        std::printf("  $%04X  %-12s  (len=%d)\n", pc, mnem.c_str(), len);
        pc += len;
    }
    return 0;
}

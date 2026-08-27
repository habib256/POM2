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

// POM2 — Z80 exerciser harness (zexdoc / zexall).
//
// Runs Frank Cringle's CP/M-hosted Z80 instruction exercisers against the
// pom2::Z80 core on a flat 64 KB RAM, with a two-call BDOS stub (C_WRITE
// and C_WRITESTR are the only functions zex uses). Each zex test block
// walks every operand combination of an instruction group, CRCs the full
// register + flag state after each execution, and compares against CRCs
// captured on real Zilog silicon — so a pass is a bit-exact flag oracle,
// undocumented X/Y (zexall) included.
//
// Usage: test_z80_zex <path-to-zexdoc.com-or-zexall.com>
// Pass = every block prints "OK" (no "ERROR"), exerciser reaches its
// "Tests complete" epilogue and warm-boots (JP $0000).

#include "Z80.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

using pom2::Z80;
using pom2::Z80Bus;

namespace {

struct FlatBus : Z80Bus {
    uint8_t ram[0x10000] = {};
    uint8_t z80MemRead(uint16_t a) override { return ram[a]; }
    void    z80MemWrite(uint16_t a, uint8_t v) override { ram[a] = v; }
};

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <zexdoc.com|zexall.com>\n", argv[0]);
        return 2;
    }

    FlatBus bus;
    FILE* fp = fopen(argv[1], "rb");
    if (!fp) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    size_t len = fread(bus.ram + 0x0100, 1, sizeof(bus.ram) - 0x0100, fp);
    fclose(fp);
    if (len < 0x100) {
        fprintf(stderr, "%s: too short (%zu bytes) — not a zex binary\n", argv[1], len);
        return 2;
    }

    // Minimal CP/M zero page: a RET at the BDOS entry so a stray CALL 5
    // that slips past the PC hook still returns, and a HALT at $0000 so
    // the warm-boot jump parks the CPU if the hook is missed.
    bus.ram[0x0000] = 0x76;   // HALT
    bus.ram[0x0005] = 0xC9;   // RET

    Z80 cpu(bus);
    cpu.reset();
    cpu.setPC(0x0100);
    cpu.setSP(0xF000);

    std::string output;
    uint64_t totalT = 0;
    // zexall retires ~46 billion T-states; 200e9 is a generous runaway cap.
    const uint64_t kMaxT = 200000000000ULL;

    while (totalT < kMaxT) {
        uint16_t pc = cpu.getPC();

        if (pc == 0x0000)      // warm boot — exerciser finished
            break;

        if (pc == 0x0005) {    // BDOS call
            const auto& s = cpu.getState();
            switch (s.c) {
            case 2:            // C_WRITE — char in E
                output.push_back(char(s.e));
                putchar(s.e);
                break;
            case 9: {          // C_WRITESTR — '$'-terminated at DE
                uint16_t addr = cpu.getDE();
                for (int guard = 0; guard < 0x10000; ++guard) {
                    char ch = char(bus.ram[addr++]);
                    if (ch == '$')
                        break;
                    output.push_back(ch);
                    putchar(ch);
                }
                break;
            }
            default:
                break;         // zex uses only 2 and 9
            }
            fflush(stdout);
            // Emulate the RET our stub byte would perform.
            uint16_t sp = cpu.getSP();
            cpu.setPC(uint16_t(bus.ram[sp] | (bus.ram[uint16_t(sp + 1)] << 8)));
            cpu.setSP(uint16_t(sp + 2));
            continue;
        }

        totalT += uint64_t(cpu.step());
        if (cpu.isHalted())
            break;
    }

    printf("\n[%llu T-states]\n", (unsigned long long)totalT);

    if (totalT >= kMaxT) {
        fprintf(stderr, "FAIL: runaway (T-state cap hit)\n");
        return 1;
    }
    if (output.find("ERROR") != std::string::npos) {
        fprintf(stderr, "FAIL: exerciser reported CRC errors\n");
        return 1;
    }
    if (output.find("Tests complete") == std::string::npos) {
        fprintf(stderr, "FAIL: exerciser did not reach 'Tests complete'\n");
        return 1;
    }
    printf("z80_zex_test: PASS\n");
    return 0;
}

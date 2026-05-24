// 6502/65C02 instruction CYCLE-COUNT test. POM2's Klaus + cmos tests pin
// instruction *results* but not their *cycle counts* — which is exactly how
// the read-modify-write undercount slipped in: INC/DEC absolute charged 5
// cycles instead of 6 (missing the RMW dummy bus cycle), drifting disk
// timing on tight RWTS loops (Mr. Robot 4am boot — found via a MAME
// cycle-trace diff, 2026-05-23). This file gates the RMW timings against
// the canonical NEC/MOS 6502 table (and MAME `om6502.lst` / `ow65c02.lst`).

#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace {

// Execute exactly one instruction at `at` (regs as-is) and return the
// cycles it consumed. run(1) runs until >=1 cycle elapsed = one opcode.
int oneInstr(M6502& cpu, Memory& mem, std::initializer_list<uint8_t> bytes,
             uint16_t at)
{
    uint16_t a = at;
    for (uint8_t b : bytes) mem.memWrite(a++, b);
    cpu.setProgramCounter(at);
    return cpu.run(1);
}

// Measure a target opcode after first setting X=$04 (for indexed modes),
// so the indexed effective address stays on-page (no spurious page cross).
int withX4(M6502& cpu, Memory& mem, std::initializer_list<uint8_t> target)
{
    mem.memWrite(0x0200, 0xA2);   // LDX #$04
    mem.memWrite(0x0201, 0x04);
    uint16_t a = 0x0202;
    for (uint8_t b : target) mem.memWrite(a++, b);
    cpu.setProgramCounter(0x0200);
    (void)cpu.run(1);             // LDX #$04
    return cpu.run(1);            // target instruction
}

struct Case { const char* name; std::initializer_list<uint8_t> code; int expect; bool idx; };

}  // namespace

int main()
{
    Memory mem;
    mem.setTestMode(true);
    M6502 cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);
    cpu.hardReset();

    // Read-modify-write: zp=5, zp,X=6, abs=6, abs,X=7 for ALL of
    // ASL/LSR/ROL/ROR/INC/DEC. INC/DEC are the ones that were wrong.
    const Case cases[] = {
        // INC ($E6/$F6/$EE/$FE)
        {"INC zp",    {0xE6, 0x40},             5, false},
        {"INC zp,X",  {0xF6, 0x40},             6, true },
        {"INC abs",   {0xEE, 0x00, 0x03},       6, false},
        {"INC abs,X", {0xFE, 0x00, 0x03},       7, true },
        // DEC ($C6/$D6/$CE/$DE)
        {"DEC zp",    {0xC6, 0x40},             5, false},
        {"DEC zp,X",  {0xD6, 0x40},             6, true },
        {"DEC abs",   {0xCE, 0x00, 0x03},       6, false},
        {"DEC abs,X", {0xDE, 0x00, 0x03},       7, true },
        // ASL ($06/$16/$0E/$1E) — already correct; pinned as control.
        {"ASL zp",    {0x06, 0x40},             5, false},
        {"ASL abs",   {0x0E, 0x00, 0x03},       6, false},
        {"ASL abs,X", {0x1E, 0x00, 0x03},       7, true },
        // ROR ($66/$6E) control.
        {"ROR zp",    {0x66, 0x40},             5, false},
        {"ROR abs",   {0x6E, 0x00, 0x03},       6, false},
        // Sanity anchors (must stay correct).
        {"LDA #imm",  {0xA9, 0x00},             2, false},
        {"NOP",       {0xEA},                   2, false},
        {"JMP abs",   {0x4C, 0x00, 0x02},       3, false},
        {"LDA abs,X", {0xBD, 0x00, 0x03},       4, true },  // no page cross
    };

    for (const Case& c : cases) {
        const int got = c.idx ? withX4(cpu, mem, c.code)
                              : oneInstr(cpu, mem, c.code, 0x0200);
        if (got != c.expect) {
            std::printf("FAIL %-10s expected %d cycles, got %d\n",
                        c.name, c.expect, got);
            assert(got == c.expect);
        }
        std::printf("%-10s = %d cycles: OK\n", c.name, got);
    }

    std::printf("cpu_cycle_count OK\n");
    return 0;
}

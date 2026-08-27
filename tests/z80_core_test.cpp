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

// POM2 — Z80 core smoke test (committed, no external data).
//
// Hand-checked spot assertions over every opcode page: main ALU flags
// (including the undocumented X/Y-from-operand CP rule), DAA, 16-bit
// ADC/SBC, CB rotates + BIT, DD/FD index forms (IXH/IXL access, (IX+d),
// DD CB write-back), the ED page (LDIR/CPIR, NEG, LD A,I), interrupts
// (IM1/IM2, EI delay, NMI) and documented T-state totals.
//
// zexdoc/zexall (z80_zex_test) are the exhaustive oracle; this test is
// the fast always-on gate that pins the core wiring and would catch a
// gross regression without a 2-minute exerciser run.

#include "Z80.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using pom2::Z80;
using pom2::Z80Bus;
using Flag = pom2::Z80::Flag;

namespace {

struct FlatBus : Z80Bus {
    uint8_t ram[0x10000] = {};
    std::vector<std::pair<uint16_t, uint8_t>> ioWrites;
    uint8_t ioReadValue = 0xA5;

    uint8_t z80MemRead(uint16_t a) override { return ram[a]; }
    void    z80MemWrite(uint16_t a, uint8_t v) override { ram[a] = v; }
    uint8_t z80IoRead(uint16_t) override { return ioReadValue; }
    void    z80IoWrite(uint16_t p, uint8_t v) override { ioWrites.push_back({p, v}); }
};

struct Rig {
    FlatBus bus;
    Z80 cpu{bus};

    Rig() { cpu.reset(); }

    /// Load `code` at $0000, run exactly `n` instructions, return total T.
    int runInstr(std::initializer_list<uint8_t> code, int n)
    {
        size_t i = 0;
        for (uint8_t b : code)
            bus.ram[i++] = b;
        cpu.setPC(0x0000);
        int total = 0;
        for (int k = 0; k < n; ++k)
            total += cpu.step();
        return total;
    }
};

} // namespace

int main()
{
    // ── 8-bit loads + ALU flags ──────────────────────────────────────
    {
        Rig r;
        // LD A,$7F ; LD B,$01 ; ADD A,B → $80: overflow, sign, half
        int t = r.runInstr({0x3E, 0x7F, 0x06, 0x01, 0x80}, 3);
        assert(r.cpu.getA() == 0x80);
        assert(r.cpu.getF() & Flag::S);
        assert(r.cpu.getF() & Flag::PV);
        assert(r.cpu.getF() & Flag::H);
        assert(!(r.cpu.getF() & (Flag::Z | Flag::C | Flag::N)));
        assert(t == 7 + 7 + 4);
    }
    {
        Rig r;
        // LD A,$00 ; SUB $01 → $FF: borrow, sign, half, N
        r.runInstr({0x3E, 0x00, 0xD6, 0x01}, 2);
        assert(r.cpu.getA() == 0xFF);
        uint8_t f = r.cpu.getF();
        assert((f & Flag::C) && (f & Flag::N) && (f & Flag::S) && (f & Flag::H));
        assert(!(f & Flag::PV));
    }
    {
        Rig r;
        // CP: X/Y come from the OPERAND ($28 = bits 5+3), not the result.
        // LD A,$40 ; CP $28
        r.runInstr({0x3E, 0x40, 0xFE, 0x28}, 2);
        uint8_t f = r.cpu.getF();
        assert((f & Flag::Y) && (f & Flag::X));
        assert(r.cpu.getA() == 0x40);
    }
    {
        Rig r;
        // AND sets H; XOR/OR clear it; parity into PV.
        r.runInstr({0x3E, 0x0F, 0xE6, 0x03}, 2);   // LD A,$0F ; AND $03 → $03
        assert(r.cpu.getA() == 0x03);
        assert(r.cpu.getF() & Flag::H);
        assert(r.cpu.getF() & Flag::PV);           // $03 = even parity
    }

    // ── INC/DEC PV edge + C preservation ─────────────────────────────
    {
        Rig r;
        // SCF ; LD A,$7F ; INC A → PV (7F→80), C survives
        r.runInstr({0x37, 0x3E, 0x7F, 0x3C}, 3);
        assert(r.cpu.getA() == 0x80);
        assert(r.cpu.getF() & Flag::PV);
        assert(r.cpu.getF() & Flag::C);
        assert(!(r.cpu.getF() & Flag::N));
    }
    {
        Rig r;
        // LD A,$80 ; DEC A → PV, N
        r.runInstr({0x3E, 0x80, 0x3D}, 2);
        assert(r.cpu.getA() == 0x7F);
        assert((r.cpu.getF() & Flag::PV) && (r.cpu.getF() & Flag::N));
    }

    // ── DAA ──────────────────────────────────────────────────────────
    {
        Rig r;
        // LD A,$15 ; ADD A,$27 → $3C ; DAA → $42 (BCD 15+27)
        r.runInstr({0x3E, 0x15, 0xC6, 0x27, 0x27}, 3);
        assert(r.cpu.getA() == 0x42);
        assert(!(r.cpu.getF() & Flag::C));
    }
    {
        Rig r;
        // LD A,$91 ; ADD A,$25 → $B6 ; DAA → $16 carry (BCD 91+25=116)
        r.runInstr({0x3E, 0x91, 0xC6, 0x25, 0x27}, 3);
        assert(r.cpu.getA() == 0x16);
        assert(r.cpu.getF() & Flag::C);
    }

    // ── 16-bit arithmetic ────────────────────────────────────────────
    {
        Rig r;
        // LD HL,$4343 ; LD BC,$1111 ; ADD HL,BC — S/Z/PV preserved-ish,
        // result $5454
        r.runInstr({0x21, 0x43, 0x43, 0x01, 0x11, 0x11, 0x09}, 3);
        assert(r.cpu.getHL() == 0x5454);
        assert(!(r.cpu.getF() & Flag::C));
    }
    {
        Rig r;
        // OR A (clear carry) ; LD HL,$8000 ; LD DE,$8000 ; ADC HL,DE →
        // $0000: Z, C, PV (overflow -32768 + -32768)
        r.runInstr({0xB7, 0x21, 0x00, 0x80, 0x11, 0x00, 0x80, 0xED, 0x5A}, 4);
        assert(r.cpu.getHL() == 0x0000);
        uint8_t f = r.cpu.getF();
        assert((f & Flag::Z) && (f & Flag::C) && (f & Flag::PV));
    }
    {
        Rig r;
        // OR A ; LD HL,$1000 ; LD BC,$0001 ; SBC HL,BC → $0FFF, N set
        r.runInstr({0xB7, 0x21, 0x00, 0x10, 0x01, 0x01, 0x00, 0xED, 0x42}, 4);
        assert(r.cpu.getHL() == 0x0FFF);
        assert(r.cpu.getF() & Flag::N);
        assert(!(r.cpu.getF() & Flag::C));
    }

    // ── rotates + CB page ────────────────────────────────────────────
    {
        Rig r;
        // LD A,$81 ; RLCA → $03, C
        r.runInstr({0x3E, 0x81, 0x07}, 2);
        assert(r.cpu.getA() == 0x03);
        assert(r.cpu.getF() & Flag::C);
    }
    {
        Rig r;
        // LD B,$81 ; SRL B (CB 38) → $40, C ; parity flag = 0 ($40 odd bits=1)
        r.runInstr({0x06, 0x81, 0xCB, 0x38}, 2);
        assert((r.cpu.getBC() >> 8) == 0x40);
        assert(r.cpu.getF() & Flag::C);
        assert(!(r.cpu.getF() & Flag::PV));
    }
    {
        Rig r;
        // LD A,$20 ; BIT 5,A → nonzero: Z clear, Y set (res bit5), H set
        r.runInstr({0x3E, 0x20, 0xCB, 0x6F}, 2);
        uint8_t f = r.cpu.getF();
        assert(!(f & Flag::Z));
        assert(f & Flag::Y);
        assert(f & Flag::H);
        // BIT 4,A → zero: Z + PV
        r.bus.ram[4] = 0xCB; r.bus.ram[5] = 0x67;
        r.cpu.setPC(4);
        r.cpu.step();
        f = r.cpu.getF();
        assert((f & Flag::Z) && (f & Flag::PV));
    }
    {
        Rig r;
        // SET 3,(HL) / RES 0,(HL)
        r.bus.ram[0x2000] = 0x01;
        r.runInstr({0x21, 0x00, 0x20, 0xCB, 0xDE, 0xCB, 0x86}, 3);
        assert(r.bus.ram[0x2000] == 0x08);
    }

    // ── index registers (DD/FD) ──────────────────────────────────────
    {
        Rig r;
        // LD IX,$3000 ; LD (IX+5),$77 ; LD A,(IX+5)
        // 3 instructions x (prefix step + opcode step) = 6 steps
        int t = r.runInstr({0xDD, 0x21, 0x00, 0x30,
                            0xDD, 0x36, 0x05, 0x77,
                            0xDD, 0x7E, 0x05}, 6);
        assert(r.cpu.getIX() == 0x3000);
        assert(r.bus.ram[0x3005] == 0x77);
        assert(r.cpu.getA() == 0x77);
        assert(t == 14 + 19 + 19);
    }
    {
        Rig r;
        // Undocumented IXH/IXL: LD IX,$1234 ; LD A,IXH ; ADD A,IXL
        r.runInstr({0xDD, 0x21, 0x34, 0x12, 0xDD, 0x7C, 0xDD, 0x85}, 6);
        assert(r.cpu.getA() == 0x12 + 0x34);
    }
    {
        Rig r;
        // LD H,(IX+d) loads REAL H, not IXH.
        r.bus.ram[0x3001] = 0xAB;
        r.runInstr({0xDD, 0x21, 0x00, 0x30, 0xDD, 0x66, 0x01}, 4);
        assert(((r.cpu.getHL() >> 8) & 0xFF) == 0xAB);
        assert(r.cpu.getIX() == 0x3000);
    }
    {
        Rig r;
        // DD CB write-back: RLC (IX+2),B → memory rotated AND copied to B
        r.bus.ram[0x3002] = 0x81;
        int t = r.runInstr({0xDD, 0x21, 0x00, 0x30, 0xDD, 0xCB, 0x02, 0x00}, 4);
        assert(r.bus.ram[0x3002] == 0x03);
        assert((r.cpu.getBC() >> 8) == 0x03);
        assert(t == 14 + 23);
        // DD CB BIT is 20 T
        r.bus.ram[8] = 0xDD; r.bus.ram[9] = 0xCB; r.bus.ram[10] = 0x02; r.bus.ram[11] = 0x46;
        r.cpu.setPC(8);
        int tBit = r.cpu.step();     // DD prefix M1 (4 T)
        tBit += r.cpu.step();        // CB d op (16 T)
        assert(tBit == 20);
    }

    // ── control flow + stack ─────────────────────────────────────────
    {
        Rig r;
        // LD B,3 ; loop: DJNZ loop → spins 3 iterations (2 taken, 1 not)
        int t = r.runInstr({0x06, 0x03, 0x10, 0xFE}, 4);
        assert((r.cpu.getBC() >> 8) == 0);
        assert(r.cpu.getPC() == 0x0004);
        assert(t == 7 + 13 + 13 + 8);
    }
    {
        Rig r;
        // CALL/RET round trip + PUSH/POP AF
        // LD SP,$F000 ; CALL $0010 ; HALT ; at $0010: LD A,$5A ; PUSH AF ;
        // POP BC ; RET
        r.bus.ram[0x10] = 0x3E; r.bus.ram[0x11] = 0x5A;   // LD A,$5A
        r.bus.ram[0x12] = 0xF5;                           // PUSH AF
        r.bus.ram[0x13] = 0xC1;                           // POP BC
        r.bus.ram[0x14] = 0xC9;                           // RET
        r.runInstr({0x31, 0x00, 0xF0, 0xCD, 0x10, 0x00}, 7);
        assert(r.cpu.getPC() == 0x0007);   // past the HALT slot
        assert((r.cpu.getBC() >> 8) == 0x5A);
        assert(r.cpu.getSP() == 0xF000);
    }
    {
        Rig r;
        // Conditional taken/not-taken timing: RET NZ (not taken 5, taken 11)
        r.runInstr({0x31, 0x00, 0xF0, 0xAF, 0xC0}, 2);   // LD SP ; XOR A
        int t = r.cpu.step();                            // RET NZ, Z set
        assert(t == 5);
    }
    {
        Rig r;
        // EX (SP),HL ; EXX ; EX DE,HL ; EX AF,AF'
        r.bus.ram[0xEFFE] = 0x78; r.bus.ram[0xEFFF] = 0x56;
        r.runInstr({0x31, 0xFE, 0xEF, 0x21, 0x34, 0x12, 0xE3}, 3);
        assert(r.cpu.getHL() == 0x5678);
        assert(r.bus.ram[0xEFFE] == 0x34 && r.bus.ram[0xEFFF] == 0x12);
    }
    {
        Rig r;
        // JP (IX)
        r.runInstr({0xDD, 0x21, 0x40, 0x00, 0xDD, 0xE9}, 4);
        assert(r.cpu.getPC() == 0x0040);
    }

    // ── ED block ops ─────────────────────────────────────────────────
    {
        Rig r;
        // LDIR: copy 4 bytes $2000→$4000
        memcpy(r.bus.ram + 0x2000, "\xDE\xAD\xBE\xEF", 4);
        r.runInstr({0x21, 0x00, 0x20,        // LD HL,$2000
                    0x11, 0x00, 0x40,        // LD DE,$4000
                    0x01, 0x04, 0x00,        // LD BC,4
                    0xED, 0xB0}, 3 + 4);     // LDIR (4 iterations)
        assert(memcmp(r.bus.ram + 0x4000, "\xDE\xAD\xBE\xEF", 4) == 0);
        assert(r.cpu.getBC() == 0);
        assert(r.cpu.getHL() == 0x2004);
        assert(!(r.cpu.getF() & Flag::PV));   // BC exhausted
    }
    {
        Rig r;
        // CPIR finds $42 at third byte
        r.bus.ram[0x2000] = 0x10; r.bus.ram[0x2001] = 0x20; r.bus.ram[0x2002] = 0x42;
        r.runInstr({0x3E, 0x42,              // LD A,$42
                    0x21, 0x00, 0x20,        // LD HL,$2000
                    0x01, 0x10, 0x00,        // LD BC,$10
                    0xED, 0xB1}, 4 + 2);     // CPIR (3 iterations)
        assert(r.cpu.getHL() == 0x2003);
        assert(r.cpu.getF() & Flag::Z);
        assert(r.cpu.getF() & Flag::PV);      // BC not exhausted
    }
    {
        Rig r;
        // NEG
        r.runInstr({0x3E, 0x01, 0xED, 0x44}, 2);
        assert(r.cpu.getA() == 0xFF);
        assert((r.cpu.getF() & Flag::N) && (r.cpu.getF() & Flag::C));
    }
    {
        Rig r;
        // LD I,A ; LD A,I → PV mirrors IFF2 (DI → clear)
        r.runInstr({0xF3, 0x3E, 0x7B, 0xED, 0x47, 0xED, 0x57}, 4);
        assert(r.cpu.getA() == 0x7B);
        assert(!(r.cpu.getF() & Flag::PV));
    }
    {
        Rig r;
        // RLD: A=$7A, (HL)=$31 → A=$73, (HL)=$1A
        r.bus.ram[0x2000] = 0x31;
        r.runInstr({0x3E, 0x7A, 0x21, 0x00, 0x20, 0xED, 0x6F}, 3);
        assert(r.cpu.getA() == 0x73);
        assert(r.bus.ram[0x2000] == 0x1A);
    }

    // ── I/O ──────────────────────────────────────────────────────────
    {
        Rig r;
        // LD A,$12 ; OUT ($34),A → port $1234 ; IN A,($56)
        r.runInstr({0x3E, 0x12, 0xD3, 0x34, 0xDB, 0x56}, 3);
        assert(r.bus.ioWrites.size() == 1);
        assert(r.bus.ioWrites[0].first == 0x1234);
        assert(r.bus.ioWrites[0].second == 0x12);
        assert(r.cpu.getA() == 0xA5);
        // IN r,(C): flags from value
        r.bus.ram[6] = 0x01; r.bus.ram[7] = 0x77; r.bus.ram[8] = 0x66;  // LD BC,$6677
        r.bus.ram[9] = 0xED; r.bus.ram[10] = 0x50;                     // IN D,(C)
        r.cpu.setPC(6);
        r.cpu.step(); r.cpu.step();
        assert(r.cpu.getDE() >> 8 == 0xA5);
        assert(r.cpu.getF() & Flag::S);
    }

    // ── interrupts ───────────────────────────────────────────────────
    {
        Rig r;
        // IM 1, EI, then assert INT: EI delays acceptance by exactly one
        // instruction; service pushes PC and jumps to $38.
        r.bus.ram[0x38] = 0x76;   // HALT at the handler
        r.runInstr({0x31, 0x00, 0xF0,   // LD SP,$F000
                    0xED, 0x56,         // IM 1
                    0xFB,               // EI
                    0x00,               // NOP (EI shadow)
                    0x00}, 3);          // stop after EI
        r.cpu.setIrqLine(true);
        r.cpu.step();                   // must execute the shadow NOP
        assert(r.cpu.getPC() == 0x0007);
        r.cpu.step();                   // now the IRQ is taken
        assert(r.cpu.getPC() == 0x0038);
        assert(r.bus.ram[0xEFFE] == 0x07);   // pushed return address
    }
    {
        Rig r;
        // IM 2 vector fetch: I=$40, bus data $10 → vector at $4010
        r.bus.ram[0x4010] = 0x00; r.bus.ram[0x4011] = 0x30;
        r.runInstr({0x31, 0x00, 0xF0,   // LD SP,$F000
                    0x3E, 0x40,         // LD A,$40
                    0xED, 0x47,         // LD I,A
                    0xED, 0x5E,         // IM 2
                    0xFB,               // EI
                    0x00}, 6);          // ... incl. shadow NOP
        r.cpu.setIrqLine(true, 0x10);
        r.cpu.step();
        assert(r.cpu.getPC() == 0x3000);
    }
    {
        Rig r;
        // NMI beats HALT and jumps to $66 with interrupts left disabled.
        r.runInstr({0x31, 0x00, 0xF0, 0x76}, 2);   // LD SP ; HALT
        assert(r.cpu.isHalted());
        r.cpu.triggerNmi();
        r.cpu.step();
        assert(!r.cpu.isHalted());
        assert(r.cpu.getPC() == 0x0066);
    }

    // ── T-state spot checks ──────────────────────────────────────────
    {
        Rig r;
        assert(r.runInstr({0x00}, 1) == 4);                    // NOP
        Rig r2;
        assert(r2.runInstr({0x3E, 0x11}, 1) == 7);             // LD A,n
        Rig r3;
        assert(r3.runInstr({0x21, 0x00, 0x10, 0x34}, 2) == 10 + 11); // INC (HL)
        Rig r4;
        assert(r4.runInstr({0xED, 0xA0}, 1) == 16);            // LDI
        Rig r5;
        assert(r5.runInstr({0xC3, 0x00, 0x10}, 1) == 10);      // JP nn
        Rig r6;
        assert(r6.runInstr({0xDD, 0x23}, 2) == 10);            // INC IX (prefix + op)
    }

    printf("z80_core_test: all assertions passed\n");
    return 0;
}

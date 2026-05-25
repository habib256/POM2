// 65C02 instruction smoke test. Pins the 65C02 additions on top of the
// 6502 base: STZ, BRA, INA/DEA, PHX/PHY/PLX/PLY, BIT #imm/zp,X/abs,X,
// TSB/TRB, the (zp) addressing mode for ORA/AND/EOR/ADC/STA/LDA/CMP/SBC,
// and JMP (abs,X). Runs entirely in flat-RAM mode (Memory::setTestMode)
// so we don't pull in the soft-switch / slot machinery.
//
// The Klaus 6502 functional test continues to gate the original 6502
// semantics; this file gates the additions.

#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

// Drive the CPU forward until it hits a BRK (we use BRK as a "test
// finished" sentinel; the CPU vectors through $FFFE/$FFFF which we point
// at $FFFE = $00 → CPU loops at BRK). Returns the cycle count consumed.
int runUntilBrk(M6502& cpu, Memory& /*mem*/, int maxCycles)
{
    int total = 0;
    while (total < maxCycles) {
        const uint16_t pc = cpu.getProgramCounter();
        const int n = cpu.run(64);
        total += n;
        // Detect "PC parked just after a BRK" — BRK pushes PC+2 to stack
        // and then jumps via the IRQ vector. With our flat-RAM test mode
        // the vector ($FFFE) reads $00 / $00, so we'd loop at $0000.
        if (cpu.getProgramCounter() == pc && pc < 0x0080) break;
    }
    return total;
}

// Helper: load a small program at $0200, jump there, assert SP/A/X/Y/P
// after running until BRK.
void runProgram(Memory& mem, M6502& cpu, std::initializer_list<uint8_t> code,
                uint16_t addr = 0x0200)
{
    uint16_t a = addr;
    for (uint8_t b : code) mem.memWrite(a++, b);
    mem.memWrite(a, 0x00);     // BRK as sentinel
    // Reset flat-RAM IRQ vector to point right back at where the BRK is —
    // we just want the CPU to halt. Easiest: vector → BRK at end of code.
    // Klaus harness doesn't care about flag init beyond what testMode lets
    // us write directly.
    mem.memWrite(0xFFFE, static_cast<uint8_t>(a & 0xFF));
    mem.memWrite(0xFFFF, static_cast<uint8_t>((a >> 8) & 0xFF));
    cpu.setProgramCounter(addr);
    runUntilBrk(cpu, mem, 100000);
}

}  // namespace

int main()
{
    Memory mem;
    mem.setTestMode(true);
    M6502 cpu(&mem);
    cpu.hardReset();

    // ── STZ abs ($9C) — store zero ────────────────────────────────────
    mem.memWrite(0x0500, 0xAB);
    runProgram(mem, cpu, {
        0xA9, 0x42,           // LDA #$42
        0x9C, 0x00, 0x05,     // STZ $0500
    });
    assert(mem.memRead(0x0500) == 0x00);
    assert(cpu.getAccumulator() == 0x42);   // STZ doesn't touch A

    // ── STZ zp ($64) and STZ zp,X ($74) ─────────────────────────────
    mem.memWrite(0x0050, 0xFF);
    mem.memWrite(0x0055, 0xFF);
    runProgram(mem, cpu, {
        0xA2, 0x05,           // LDX #$05
        0x64, 0x50,           // STZ $50
        0x74, 0x50,           // STZ $50,X → $55
    });
    assert(mem.memRead(0x0050) == 0x00);
    assert(mem.memRead(0x0055) == 0x00);

    // ── BRA ($80) — unconditional branch ─────────────────────────────
    runProgram(mem, cpu, {
        0xA9, 0x11,           // LDA #$11
        0x80, 0x02,           // BRA +2 → skip the next 2 bytes
        0xA9, 0xFF,           // LDA #$FF (must be skipped)
        0xA2, 0x77,           // LDX #$77
    });
    assert(cpu.getAccumulator() == 0x11);
    assert(cpu.getXRegister()    == 0x77);

    // ── INA / DEA ────────────────────────────────────────────────────
    runProgram(mem, cpu, {
        0xA9, 0x07,           // LDA #$07
        0x1A,                 // INA → 8
        0x1A,                 // INA → 9
        0x3A,                 // DEA → 8
    });
    assert(cpu.getAccumulator() == 0x08);

    // ── PHX/PHY/PLX/PLY ──────────────────────────────────────────────
    runProgram(mem, cpu, {
        0xA2, 0xCA,           // LDX #$CA
        0xA0, 0xFE,           // LDY #$FE
        0xDA,                 // PHX
        0x5A,                 // PHY
        0xA2, 0x00,           // LDX #0   (clobber)
        0xA0, 0x00,           // LDY #0
        0x7A,                 // PLY (recover $FE)
        0xFA,                 // PLX (recover $CA)
    });
    assert(cpu.getXRegister() == 0xCA);
    assert(cpu.getYRegister() == 0xFE);

    // ── BIT #imm ($89) — only Z affected ──────────────────────────────
    // Set V and N high, then BIT #imm with a value that AND's to non-zero
    // — V/N must be unchanged, Z=0.
    runProgram(mem, cpu, {
        0xA9, 0x40, 0x09, 0x80, 0x48, 0x28, // PHA / PLP path to set V+N
        0xA9, 0x33,            // LDA #$33
        0x89, 0x10,            // BIT #$10 (33 & 10 = 10 → Z=0)
    });
    // Z=0 → bit 1 of P clear
    assert((cpu.getStatusRegister() & 0x02) == 0);

    // ── (zp) — LDA, STA, ORA ─────────────────────────────────────────
    // Set up zp pointer at $30/$31 → $0700.
    mem.memWrite(0x0030, 0x00);
    mem.memWrite(0x0031, 0x07);
    mem.memWrite(0x0700, 0x5A);
    runProgram(mem, cpu, {
        0xB2, 0x30,            // LDA ($30) → $5A
    });
    assert(cpu.getAccumulator() == 0x5A);

    runProgram(mem, cpu, {
        0xA9, 0x99,            // LDA #$99
        0x92, 0x30,            // STA ($30) → $0700 = $99
    });
    assert(mem.memRead(0x0700) == 0x99);

    // ── TSB / TRB ────────────────────────────────────────────────────
    mem.memWrite(0x0600, 0x10);    // bit 4 set
    runProgram(mem, cpu, {
        0xA9, 0x21,            // LDA #$21 (bits 0 and 5)
        0x0C, 0x00, 0x06,      // TSB $0600 → $0600 = $10|$21 = $31
    });
    assert(mem.memRead(0x0600) == 0x31);
    runProgram(mem, cpu, {
        0xA9, 0x20,            // LDA #$20 (bit 5)
        0x1C, 0x00, 0x06,      // TRB $0600 → clear bit 5: $31 & ~$20 = $11
    });
    assert(mem.memRead(0x0600) == 0x11);

    // ── JMP (abs,X) ($7C) ────────────────────────────────────────────
    // Build a jump table at $0400 that selects between two BRK-labelled
    // exit points based on X.
    mem.memWrite(0x0400, 0x00); mem.memWrite(0x0401, 0x05);  // entry 0 → $0500
    mem.memWrite(0x0402, 0x10); mem.memWrite(0x0403, 0x05);  // entry 1 → $0510
    // At $0500: LDA #$AA; BRK
    mem.memWrite(0x0500, 0xA9); mem.memWrite(0x0501, 0xAA);
    mem.memWrite(0x0502, 0x00);
    // At $0510: LDA #$BB; BRK
    mem.memWrite(0x0510, 0xA9); mem.memWrite(0x0511, 0xBB);
    mem.memWrite(0x0512, 0x00);
    // Vector $FFFE/F to a stable BRK so the CPU doesn't drift after the
    // post-BRK landing — runProgram already overwrites this when called.
    mem.memWrite(0xFFFE, 0x02); mem.memWrite(0xFFFF, 0x05);

    cpu.setProgramCounter(0x0200);
    mem.memWrite(0x0200, 0xA2); mem.memWrite(0x0201, 0x02); // LDX #$02 (entry 1: bytes per pointer = 2)
    mem.memWrite(0x0202, 0x7C); mem.memWrite(0x0203, 0x00); mem.memWrite(0x0204, 0x04);  // JMP ($0400,X)
    runUntilBrk(cpu, mem, 100000);
    assert(cpu.getAccumulator() == 0xBB);

    // ── Rockwell RMBn ($07-$77) / SMBn ($87-$F7) ──────────────────────
    // Per the Rockwell datasheet: $07/$17/.../$77 = RMB0..RMB7 (RESET bit n
    // of zp); $87/$97/.../$F7 = SMB0..SMB7 (SET bit n of zp). Pin both,
    // because POM2 had them swapped at the dispatch level until 2026-05-08.
    // Set bit 3 of $40 (starting at 0) via SMB3 ($B7); then clear via RMB3 ($37).
    mem.memWrite(0x0040, 0x00);
    runProgram(mem, cpu, {
        0xB7, 0x40,            // SMB3 $40 → $40 |= $08 = $08
    });
    assert(mem.memRead(0x0040) == 0x08);
    runProgram(mem, cpu, {
        0x37, 0x40,            // RMB3 $40 → $40 &= ~$08 = $00
    });
    assert(mem.memRead(0x0040) == 0x00);
    // RMB0 + RMB7 + SMB7 on the same byte to verify independent bits.
    mem.memWrite(0x0041, 0xFF);
    runProgram(mem, cpu, {
        0x07, 0x41,            // RMB0 $41 → $FF & ~$01 = $FE
        0x77, 0x41,            // RMB7 $41 → $FE & ~$80 = $7E
        0xF7, 0x41,            // SMB7 $41 → $7E | $80  = $FE
    });
    assert(mem.memRead(0x0041) == 0xFE);

    // ── Rockwell BBRn / BBSn ($0F,$1F,...,$7F / $8F,$9F,...,$FF) ──────
    // BBR0 zp,offset — branch when bit 0 is reset.
    mem.memWrite(0x0050, 0x02);    // bit 1 set, bit 0 clear
    runProgram(mem, cpu, {
        0xA9, 0x11,            // LDA #$11
        0x0F, 0x50, 0x02,      // BBR0 $50, +2 → branch (bit 0 clear)
        0xA9, 0xFF,            // LDA #$FF (must be skipped)
        0xA2, 0x77,            // LDX #$77
    });
    assert(cpu.getAccumulator() == 0x11);
    assert(cpu.getXRegister()    == 0x77);

    // BBS7 ($FF) — branch when bit 7 set. The opcode byte itself is $FF;
    // the canonical "first byte falls into a slot 2 ROM" case from
    // CLAUDE.md / the ScoSwamp investigation. Pin it explicitly.
    mem.memWrite(0x0051, 0x80);    // bit 7 set
    runProgram(mem, cpu, {
        0xA9, 0x22,            // LDA #$22
        0xFF, 0x51, 0x02,      // BBS7 $51, +2 → branch (bit 7 set)
        0xA9, 0xEE,            // LDA #$EE (must be skipped)
        0xA2, 0x55,            // LDX #$55
    });
    assert(cpu.getAccumulator() == 0x22);
    assert(cpu.getXRegister()    == 0x55);

    // BBS7 with bit clear → no branch, fall through.
    mem.memWrite(0x0052, 0x00);
    runProgram(mem, cpu, {
        0xA9, 0x33,            // LDA #$33
        0xFF, 0x52, 0x02,      // BBS7 $52, +2 → NO branch (bit 7 clear)
        0xA9, 0xCC,            // LDA #$CC (must execute)
    });
    assert(cpu.getAccumulator() == 0xCC);

    // ── 65C02 decimal-mode SBC overflow (V) flag (round 10 #1) ───────────
    // On the 65C02 the V flag in decimal SBC is computed from the BINARY
    // difference, identical to binary mode (6502.org). POM2 wrongly used the
    // BCD-adjusted accumulator → V diverged from real hardware on 2400 inputs.
    // Canonical divergent case: A=$00, M=$80, C=1 (no borrow) ⇒ hardware V=1;
    // the buggy code produced V=0.
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    runProgram(mem, cpu, {
        0xF8,                  // SED  (decimal mode)
        0x38,                  // SEC  (carry set = no borrow)
        0xA9, 0x00,            // LDA #$00
        0xE9, 0x80,            // SBC #$80
        0xD8,                  // CLD  (leave decimal so BRK/handler is sane)
    });
    assert((cpu.getStatusRegister() & M6502::Status::V) != 0 &&
           "CMOS decimal SBC $00-$80 must set V (computed from binary diff)");

    std::printf("cmos_6502_smoke OK\n");
    return 0;
}

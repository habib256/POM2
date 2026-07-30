// Decimal-mode SBC pin — the NMOS/CMOS split in `M6502::SBC`.
//
// The WDC 65C02 lets the low nibble's `-6` decimal adjustment BORROW INTO the
// high nibble; MAME names this in `w65c02.cpp:28-46` (do_sbc_cd): "SBC allows
// interdigit carry from decimal adjustment on 65C02". The NMOS part corrects
// each nibble in isolation and never propagates that borrow, so the two CPUs
// return DIFFERENT accumulators for the same operands. POM2 modelled only the
// NMOS rule for both, which cost ~3.4% of every decimal SBC addressing mode on
// Tom Harte `wdc65c02/v1/{e1,e5,e9,ed,f1,f2,f5,f9,fd}`.
//
// Vectors below are lifted verbatim from that corpus (`e9.json`, SBC #imm,
// D=1) so this file pins the real silicon contract without needing the 1.4 GB
// download that `tomharte_65c02` is gated behind:
//
//   * kInterdigit — cases the pre-fix nibble-isolated code got wrong. Each
//     expected value differs from the old result by exactly $10, the dropped
//     inter-nibble borrow.
//   * kValidBcd   — valid-BCD operands, where both rules agree. Guards against
//     "fixing" the edge case by breaking the common path.
//   * kNmosVsCmos — the same operands under BOTH cpu modes, with the two
//     distinct expected accumulators. This is the pin that actually fails if
//     the branches are ever cross-wired.
//
// Flags are checked too: N/Z come from the adjusted accumulator on CMOS, and
// V/C are the binary-difference overflow/borrow on both parts.

#include "M6502.h"
#include "Memory.h"

#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;

// N V D Z C. B/unused(bit 5) are phantom (not real latches), and I is excluded
// because these vectors' initial I is not reproduced here — SBC never writes it.
constexpr uint8_t kFlagMask = 0xCB;

struct Vec { uint8_t a, operand, carryIn, expA, expP; };

// SBC #operand with D=1 and the given carry-in, in the given CPU mode.
// Returns the resulting accumulator; `outP` gets the status register.
uint8_t runSbcImm(Memory& mem, M6502& cpu, M6502::CpuMode mode,
                  uint8_t a, uint8_t operand, uint8_t carryIn, uint8_t& outP)
{
    cpu.setCpuMode(mode);
    mem.memWrite(0x0200, 0xE9);         // SBC #imm
    mem.memWrite(0x0201, operand);
    cpu.setProgramCounter(0x0200);
    cpu.setAccumulator(a);
    // D set; carry-in per the vector. I is irrelevant (no IRQ line driven).
    cpu.setStatusRegister(static_cast<uint8_t>(M6502::Status::D |
                                               (carryIn ? M6502::Status::C : 0)));
    cpu.run(1);
    outP = cpu.getStatusRegister();
    return cpu.getAccumulator();
}

void checkSet(Memory& mem, M6502& cpu, M6502::CpuMode mode, const char* modeName,
              const char* label, const Vec* v, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        uint8_t gotP = 0;
        const uint8_t gotA = runSbcImm(mem, cpu, mode, v[i].a, v[i].operand,
                                       v[i].carryIn, gotP);
        const bool aOk = (gotA == v[i].expA);
        const bool pOk = ((gotP ^ v[i].expP) & kFlagMask) == 0;
        if (!aOk || !pOk) {
            ++failures;
            std::printf("  FAIL %s/%s: SBC #$%02X with A=$%02X C=%u -> "
                        "A got $%02X want $%02X | P got $%02X want $%02X\n",
                        modeName, label, v[i].operand, v[i].a, v[i].carryIn,
                        gotA, v[i].expA, gotP & kFlagMask, v[i].expP & kFlagMask);
        }
    }
}

// ── WDC 65C02, D=1: interdigit-carry cases (pre-fix code returned expA+$10) ──
constexpr Vec kInterdigit[] = {
    { 0x10, 0xFC, 0x01, 0xAE, 0xBC },
    { 0xB0, 0x5F, 0x00, 0x4A, 0x79 },
    { 0x71, 0x2C, 0x00, 0x3E, 0x3D },
    { 0x34, 0x4F, 0x00, 0x7E, 0x38 },
    { 0xC3, 0xCF, 0x01, 0x8E, 0xB8 },
    { 0xF0, 0x2A, 0x00, 0xBF, 0xBD },
    { 0x72, 0xAE, 0x01, 0x5E, 0x7C },
    { 0x20, 0x0E, 0x01, 0x0C, 0x39 },
};

// ── WDC 65C02, D=1: valid BCD — both correction rules agree here ──
constexpr Vec kValidBcd[] = {
    { 0x85, 0x29, 0x01, 0x56, 0x7D },
    { 0x02, 0x02, 0x00, 0x99, 0xBC },
    { 0x51, 0x75, 0x01, 0x76, 0x3C },
    { 0x97, 0x66, 0x00, 0x30, 0x79 },
};

// ── Same operands, both CPUs: the accumulators MUST differ ──
struct Split { uint8_t a, operand, carryIn, nmosA, cmosA; };
constexpr Split kNmosVsCmos[] = {
    { 0x91, 0x3D, 0x01, 0x5E, 0x4E },
    { 0x32, 0xAC, 0x00, 0x2F, 0x1F },
    { 0x32, 0x7C, 0x00, 0x5F, 0x4F },
    { 0x75, 0x2F, 0x00, 0x4F, 0x3F },
    { 0x60, 0x8B, 0x00, 0x7E, 0x6E },
    { 0x20, 0x1B, 0x00, 0x0E, 0xFE },
};

}  // namespace

int main()
{
    Memory mem;
    mem.setTestMode(true);
    M6502 cpu(&mem);
    cpu.hardReset();

    std::printf("[decimal_sbc_cmos] WDC 65C02 interdigit-carry vectors (%zu)\n",
                sizeof(kInterdigit) / sizeof(kInterdigit[0]));
    checkSet(mem, cpu, M6502::CpuMode::CMOS, "cmos", "interdigit",
             kInterdigit, sizeof(kInterdigit) / sizeof(kInterdigit[0]));

    std::printf("[decimal_sbc_cmos] WDC 65C02 valid-BCD guard vectors (%zu)\n",
                sizeof(kValidBcd) / sizeof(kValidBcd[0]));
    checkSet(mem, cpu, M6502::CpuMode::CMOS, "cmos", "valid-bcd",
             kValidBcd, sizeof(kValidBcd) / sizeof(kValidBcd[0]));

    std::printf("[decimal_sbc_cmos] NMOS-vs-CMOS divergence vectors (%zu)\n",
                sizeof(kNmosVsCmos) / sizeof(kNmosVsCmos[0]));
    for (const Split& s : kNmosVsCmos) {
        uint8_t p = 0;
        const uint8_t nmos = runSbcImm(mem, cpu, M6502::CpuMode::NMOS,
                                       s.a, s.operand, s.carryIn, p);
        const uint8_t cmos = runSbcImm(mem, cpu, M6502::CpuMode::CMOS,
                                       s.a, s.operand, s.carryIn, p);
        if (nmos != s.nmosA || cmos != s.cmosA) {
            ++failures;
            std::printf("  FAIL split: SBC #$%02X with A=$%02X C=%u -> "
                        "nmos got $%02X want $%02X, cmos got $%02X want $%02X\n",
                        s.operand, s.a, s.carryIn, nmos, s.nmosA, cmos, s.cmosA);
        }
        // The whole point of the split: the two parts must NOT agree.
        if (s.nmosA == s.cmosA) {
            ++failures;
            std::printf("  FAIL split table: vector A=$%02X op=$%02X is not "
                        "actually divergent\n", s.a, s.operand);
        }
    }

    if (failures) {
        std::printf("[decimal_sbc_cmos] FAIL: %d mismatch(es)\n", failures);
        return 1;
    }
    std::printf("[decimal_sbc_cmos] OK\n");
    return 0;
}

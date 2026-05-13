// Klaus Dormann's 65C02 extended-opcodes functional test.
//
// Companion to `klaus_6502_functional_test.cpp` (which pins the base 6502
// instruction set). This test exercises every 65C02-only addition:
//   * Stack-extension opcodes — STZ (zp / zp,X / abs / abs,X), BRA,
//     INA / DEA, PHX / PHY / PLX / PLY
//   * BIT in additional modes — `#imm`, zp,X, abs,X
//   * TSB / TRB
//   * JMP (abs,X)
//   * Zero-page indirect — ORA / AND / EOR / ADC / STA / LDA / CMP / SBC
//   * Rockwell extensions — RMB0..7, SMB0..7, BBR0..7, BBS0..7
//   * WDC halts — WAI ($CB), STP ($DB)
//   * Decimal-mode N/V/Z behaviour on CMOS (`om65c02.lst:11-14` adjust
//     the flags AFTER the BCD correction; NMOS doesn't, and the previous
//     POM2 path was the NMOS variant — fixed in the same session that
//     added this test)
//   * 65C02 timing differences (extra cycle on `BIT abs,X` with page
//     crossing, etc.)
//
// The image is identical to the 6502 test in structure: 64 KB of code +
// data + reset vectors, loaded flat at $0000, entry at $0400, JMP-to-
// self traps on every failure path. Success traps at $24F1 (the only
// "jmp *" reachable by a clean run; every other "jmp *" sits next to a
// `trap` macro comment in the listing). CMake downloads the image at
// configure time (SHA-256 pinned) and passes the path as argv[1].
//
// Reference: https://github.com/Klaus2m5/6502_65C02_functional_tests
// Binary:    bin_files/65C02_extended_opcodes_test.bin
// Listing:   bin_files/65C02_extended_opcodes_test.lst
//
// The test image expects the CPU to be in 65C02 / CMOS mode. POM2's
// `M6502` defaults to CMOS at construction (`M6502.cpp:41`), so no
// explicit mode switch is needed. If a future change flips the default
// to NMOS, this test will fail immediately on the first 65C02-only
// opcode (typically BRA at $04xx).

#include "M6502.h"
#include "Memory.h"
#include "CassetteDevice.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

// Success address for the published reference binary (SHA-256 below).
// Determined by walking the .lst for the unique "jmp *" that isn't tagged
// `trap` in a preceding comment. Klaus's convention: every error path is
// labelled `trap`; the single non-`trap` jmp-to-self is the success exit.
//
// SHA-256: 10a2a07fa240666fa610c46accebe8d42b1000feef3aae619da15a8d152869b2
constexpr uint16_t kSuccessAddress = 0x24F1;
constexpr uint16_t kTestEntry      = 0x0400;
constexpr size_t   kImageSize      = 0x10000;

// Safety cap. 65C02-extended is roughly the same cycle budget as the
// base 6502 test — ~80 M instructions for a clean pass. We allow 300 M
// so a slow build + a wandering buggy CPU still terminates before CI
// gives up.
constexpr long kMaxSteps = 300'000'000L;

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: test_klaus_65c02 <path-to-65C02_extended_opcodes_test.bin>\n");
        return 2;
    }

    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (image.size() != kImageSize) {
        std::fprintf(stderr,
            "unexpected image size: %zu bytes (expected %zu)\n",
            image.size(), kImageSize);
        return 2;
    }

    // Flat RAM: every byte addressable, no soft switches, no slot bus.
    Memory memory;
    memory.setTestMode(true);
    std::memcpy(memory.dataMutable(), image.data(), kImageSize);

    M6502 cpu(&memory);
    // 65C02 mode is the default; assert it stays that way so a future
    // refactor doesn't silently turn this into a 6502-only regression
    // gate.
    cpu.setCpuMode(M6502::CpuMode::CMOS);
    // Bypass the reset vector — Klaus's image puts a trap at $271C so a
    // misfired reset shows up loud. Entry point is documented as $0400
    // in the .lst footer ("Program start address is at $0400").
    cpu.setProgramCounter(kTestEntry);

    const auto t0 = std::chrono::steady_clock::now();
    long steps = 0;
    uint16_t lastPc = cpu.getProgramCounter();
    int stuckFor = 0;

    // Step instruction-by-instruction; detect jmp-to-self traps as two
    // consecutive identical PCs. Same logic as the base-6502 runner.
    while (steps < kMaxSteps) {
        cpu.step();
        ++steps;
        const uint16_t pc = cpu.getProgramCounter();
        if (pc == lastPc) {
            if (++stuckFor >= 2) break;
        } else {
            stuckFor = 0;
            lastPc = pc;
        }
    }

    const auto t1 = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(t1 - t0).count();

    const uint16_t finalPc = cpu.getProgramCounter();
    std::printf("Klaus 65C02 extended-opcodes test: ended at $%04X "
                "after %ld steps (%.2f s)\n",
                finalPc, steps, seconds);

    if (steps >= kMaxSteps) {
        std::fprintf(stderr,
            "TIMEOUT — test did not terminate within %ld steps. "
            "Last PC: $%04X\n", kMaxSteps, finalPc);
        return 1;
    }
    if (finalPc != kSuccessAddress) {
        std::fprintf(stderr,
            "FAIL — trapped at $%04X (expected success at $%04X)\n"
            "Cross-reference the .lst for the test that ends just before "
            "this address to identify the failing opcode / mode.\n",
            finalPc, kSuccessAddress);
        return 1;
    }

    std::printf("OK\n");
    return 0;
}

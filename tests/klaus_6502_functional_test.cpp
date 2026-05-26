// Klaus Dormann's 6502 functional test — smoke test for POM2's M6502 core.
//
// The test image exercises every official 6502 instruction + addressing
// mode, verifies all flag behaviours, and ends by JMP-ing to itself at
// a known "success" address. Any earlier trap (JMP-to-self at a different
// address) means the emulator diverged from the expected behaviour on
// that instruction.
//
// Reference: https://github.com/Klaus2m5/6502_65C02_functional_tests
//
// CMake downloads `6502_functional_test.bin` at configure time (SHA-256
// pinned) and passes the path as argv[1]. The test loads the 64 KB image
// flat at $0000 and starts execution at $0400.

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

// Success address for the published reference binary.
// SHA256: fa12bfc761e6f9057e4cc01a665a7b800ff01ae91f598af1e39a1201d01953fd
constexpr uint16_t kSuccessAddress = 0x3469;
constexpr uint16_t kTestEntry      = 0x0400;
constexpr size_t   kImageSize      = 0x10000;

// Safety cap. Klaus runs ~96 M cycles; cap at 200 M instructions so a
// buggy build can't hang CI. At native interpreter speed the test
// completes in 2-4 wallclock seconds.
constexpr long kMaxSteps = 200'000'000L;

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr,
            "usage: test_klaus_6502 <path-to-6502_functional_test.bin>\n");
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

    // Memory in flat-RAM test mode: every byte addressable as plain RAM,
    // no soft switches, no slot bus, no ROM write protect.
    Memory memory;
    memory.setTestMode(true);
    memory.loadFlatTestImage(image.data(), kImageSize);

    M6502 cpu(&memory);
    // The image's reset vector points at $0400 — set PC explicitly so we
    // don't rely on the test bypass path through the soft-switch code.
    cpu.setProgramCounter(kTestEntry);

    const auto t0 = std::chrono::steady_clock::now();
    long steps = 0;
    uint16_t lastPc = cpu.getProgramCounter();
    int stuckFor = 0;

    // Step instruction-by-instruction so JMP-to-self traps show up the
    // moment they happen. Klaus's traps are all `JMP *` — one step and
    // PC stays put. Two consecutive identical PCs confirm the trap; a
    // legitimate tight branch loop wouldn't repeat byte-for-byte.
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
    std::printf("Klaus 6502 functional test: ended at $%04X "
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
            "FAIL — trapped at $%04X (expected success at $%04X)\n",
            finalPc, kSuccessAddress);
        return 1;
    }

    std::printf("OK\n");
    return 0;
}

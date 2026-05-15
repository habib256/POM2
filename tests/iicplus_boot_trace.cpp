// Boot-trace smoke test for the //c+ profile. Loads the user's
// apple2cp.rom, applies the IIcPlus profile-equivalent memory state,
// hard-resets, runs the CPU for ~1.5 emulated seconds and reports:
//   * PC at the end (so we can see where the boot routine settles)
//   * a histogram of which $C0xx soft switches were touched
//   * a fingerprint of text page 1 ($0400-$07FF) so we can tell
//     whether the //c+ Monitor cleared the screen and printed
//     anything readable
//
// This is a diagnostic tool — it doesn't assert anything, it prints.
// The CMake target is `test_iicplus_boot_trace`; run from the build
// directory while we figure out why the //c+ refuses to boot a 5.25"
// disk via the library-click path.

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "Logger.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;
        if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p;
        if (fs::exists(up2)) return up2;
    }
    return {};
}

}

int main()
{
    Memory mem;
    M6502  cpu(&mem);

    const auto& cfg = pom2::profileConfig(pom2::SystemProfile::AppleIIcPlus);

    // Mimic applyProfile: clear RAM, reset switches, enable IIe paging,
    // load ROM with pickLowerHalf=true for //c-style banks, set CPU
    // mode, hard reset.
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);

    const std::string romPath = firstExisting({
        "roms/apple2cp.rom", "roms/apple2c-plus.rom",
        "roms/apple2c-32Kv0.rom"
    });
    if (romPath.empty()) {
        std::printf("SKIP iicplus_boot_trace: no //c+ ROM found\n");
        return 0;
    }
    if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/true)) {
        std::printf("FAIL: could not load %s\n", romPath.c_str());
        return 1;
    }

    // Plug a Disk II in slot 6 (POM2's //c+ profile does the same).
    const std::string diskRom = firstExisting({"roms/disk2.rom"});
    auto card = std::make_unique<DiskIICard>(6);
    if (!diskRom.empty()) card->loadBootRom(diskRom);
    mem.slotBus().plug(6, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    std::printf("Initial PC after reset: $%04X (expect $FA62 for //c+)\n",
                cpu.getProgramCounter());

    // Run ~1.5 emulated seconds at 4 MHz = ~6M cycles. Enough for any
    // banner display + slot scan.
    constexpr int kBudget = 6'000'000;
    int    cycles = 0;
    uint16_t pcHistory[8] = {};
    int pcHistoryIdx = 0;
    while (cycles < kBudget) {
        const uint16_t pc = cpu.getProgramCounter();
        pcHistory[pcHistoryIdx] = pc;
        pcHistoryIdx = (pcHistoryIdx + 1) & 7;
        cpu.step();
        cycles += cpu.getCurrentInstructionCycles();
    }

    std::printf("After ~6M cycles: PC=$%04X\n", cpu.getProgramCounter());
    std::printf("Last 8 PCs (newest at the right): ");
    for (int i = 0; i < 8; ++i) {
        std::printf("$%04X ", pcHistory[(pcHistoryIdx + i) & 7]);
    }
    std::printf("\n");

    // Fingerprint text page 1: count non-$00 bytes, max/min, sample first 80.
    int nonZero = 0;
    uint8_t firstChars[40] = {};
    for (uint16_t a = 0x0400; a <= 0x07FF; ++a) {
        const uint8_t b = mem.memRead(a);
        if (b != 0x00) ++nonZero;
    }
    // Top row of the screen (interleaved Apple II addressing — row 0 = $400-$427).
    for (int col = 0; col < 40; ++col) {
        firstChars[col] = mem.memRead(static_cast<uint16_t>(0x0400 + col));
    }
    std::printf("Text page 1: %d/1024 non-zero bytes\n", nonZero);
    std::printf("Row 0 raw bytes: ");
    for (int i = 0; i < 40; ++i) std::printf("%02X ", firstChars[i]);
    std::printf("\nRow 0 as ASCII: '");
    for (int i = 0; i < 40; ++i) {
        const char c = static_cast<char>(firstChars[i] & 0x7F);
        std::putchar(c >= 0x20 && c < 0x7F ? c : '.');
    }
    std::printf("'\n");

    std::printf("Done.\n");
    return 0;
}

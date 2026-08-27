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

// POM2 — dump the //c reset vector and the $C600 slot-ROM window.
//   cd build && cmake .. && make iic_dump && ./tests/iic_dump
// Diagnostic dump, not a test: it prints and asserts nothing, so it is
// EXCLUDE_FROM_ALL and outside ctest. iic_onboard_smartport gates the
// //c boot path.

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "Logger.h"
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

std::string firstExisting(const std::vector<std::string>& candidates) {
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        const std::string up1 = "../" + p;  if (fs::exists(up1)) return up1;
        const std::string up2 = "../../" + p; if (fs::exists(up2)) return up2;
    }
    return {};
}

int main() {
    Memory mem; M6502 cpu(&mem);
    mem.clearRam(); mem.resetSoftSwitches(); mem.setIIEMode(true);

    const std::string romPath = firstExisting({"roms/apple2c-32Kv0.rom"});
    if (!mem.loadAppleIIRom(romPath.c_str(), /*pickLowerHalf=*/true)) return 1;

    auto card = std::make_unique<DiskIICard>(6);
    card->loadBootRom(firstExisting({"roms/disk2.rom"}));
    card->loadLssRom(firstExisting({"roms/diskii_p6.rom"}));
    card->insertDisk(firstExisting({"disks_5.4/dsk/dos33_master.dsk"}));
    mem.slotBus().plug(6, std::move(card));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    std::printf("After reset: PC=$%04X\n", cpu.getProgramCounter());
    // Dump key addresses
    std::printf("$C600 bytes: ");
    for (uint16_t a = 0xC600; a < 0xC610; ++a) std::printf("%02X ", mem.memRead(a));
    std::printf("\n");
    std::printf("$C700 bytes: ");
    for (uint16_t a = 0xC700; a < 0xC710; ++a) std::printf("%02X ", mem.memRead(a));
    std::printf("\n");
    std::printf("$FA62 bytes: ");
    for (uint16_t a = 0xFA62; a < 0xFA82; ++a) std::printf("%02X ", mem.memRead(a));
    std::printf("\n");
    std::printf("$FFFC reset vector: %02X %02X\n",
                mem.memRead(0xFFFC), mem.memRead(0xFFFD));

    // Step the CPU and trace the first 200 instructions to see where the boot path goes.
    constexpr int kInstrs = 2000;
    for (int i = 0; i < kInstrs; ++i) {
        const uint16_t pc = cpu.getProgramCounter();
        cpu.step();
        if (i < 200 || pc == 0xC600 || pc == 0xFA62 || pc == 0xFAA6 || pc == 0xFABA || pc == 0xFAD7) {
            std::printf("[%4d] PC=$%04X\n", i, pc);
        }
    }
    std::printf("Final PC: $%04X\n", cpu.getProgramCounter());
    return 0;
}

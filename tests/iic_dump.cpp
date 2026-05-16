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
    card->insertDisk(firstExisting({"disks/dsk/dos33_master.dsk"}));
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

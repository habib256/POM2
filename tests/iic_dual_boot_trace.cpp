// Diagnostic (EXCLUDE_FROM_ALL): //c autostart with BOTH a slot-6 Disk II
// disk AND an on-board SmartPort (slot 5) holding media garbles the boot
// banner. This trace boots that exact combo headless and dumps where the
// CPU settled + the text page line 0 + the 80col/ALTCHAR soft-switch state,
// so we can see what the //c ROM did. See project_iic_smartport_boot.

#include "M6502.h"
#include "Memory.h"
#include "SystemProfile.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "IWMDevice.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "Logger.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {
std::string firstExisting(const std::vector<std::string>& c) {
    namespace fs = std::filesystem;
    for (const auto& p : c) {
        if (fs::exists(p)) return p;
        if (fs::exists("../" + p)) return "../" + p;
        if (fs::exists("../../" + p)) return "../../" + p;
    }
    return {};
}
}

int main(int argc, char** argv)
{
    const bool withSp = !(argc > 1 && std::string(argv[1]) == "nosp");

    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;
    mem.setIWM(&iwm);
    mem.setIWMAuthoritative(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);

    const std::string rom = firstExisting({"roms/apple2c-32Kv0.rom"});
    if (rom.empty()) { std::printf("SKIP: no //c ROM\n"); return 0; }
    if (!mem.loadAppleIIRom(rom.c_str(), true)) { std::printf("FAIL rom\n"); return 1; }

    // Slot 6 Disk II + a ProDOS 5.25.
    auto d2 = std::make_unique<DiskIICard>(6);
    const std::string dr = firstExisting({"roms/disk2.rom"}); if (!dr.empty()) d2->loadBootRom(dr);
    const std::string lr = firstExisting({"roms/diskii_p6.rom"}); if (!lr.empty()) d2->loadLssRom(lr);
    const std::string d5 = firstExisting({"disks_5.4/dsk/ProDOS_2_4_3.po"});
    if (!d5.empty()) d2->insertDisk(d5);
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    // Slot 5 on-board SmartPort + a bootable 3.5 (optional via argv).
    if (withSp) {
        auto sp = std::make_unique<pom2::SmartPortCard>(5);
        sp->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
        const std::string s35 = firstExisting({"disks_3.5/TheBestGames.2mg"});
        std::string err;
        if (!s35.empty() && !sp->mountBay(0, s35, err))
            std::printf("WARN: 3.5 mount: %s\n", err.c_str());
        mem.slotBus().plug(5, std::move(sp));
    }

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    std::printf("config: Disk II(slot6)=%s  SmartPort(slot5)=%s\n",
                d5.empty() ? "(none)" : "ProDOS.po", withSp ? "3.5" : "(none)");

    constexpr int kInstrs = 8'000'000;
    for (int i = 0; i < kInstrs; ++i) cpu.step();

    const uint16_t pc = cpu.getProgramCounter();
    // Soft-switch state (read side): $C01E RDALTCHAR, $C01F RD80COL, $C018 RD80STORE.
    const uint8_t altchar = mem.memRead(0xC01E);
    const uint8_t col80   = mem.memRead(0xC01F);
    const uint8_t store80 = mem.memRead(0xC018);
    std::printf("finalPC=$%04X  RDALTCHAR=$%02X RD80COL=$%02X RD80STORE=$%02X\n",
                pc, altchar, col80, store80);

    // Text page line 0 = $0400-$0427 (main). Dump hex + ASCII (strip hi bit).
    std::printf("text $0400 line0: ");
    for (int i = 0; i < 40; ++i) std::printf("%02X ", mem.memRead(0x0400 + i));
    std::printf("\n              ascii: ");
    for (int i = 0; i < 40; ++i) {
        uint8_t b = mem.memRead(0x0400 + i) & 0x7F;
        std::printf("%c", (b >= 0x20 && b < 0x7F) ? b : '.');
    }
    std::printf("\n");
    return 0;
}

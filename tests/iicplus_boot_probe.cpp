// Diagnostic (EXCLUDE_FROM_ALL): //c+ cold boot, headless — the full
// on-board stack (IWM + SmartPortHub + 2x Sony 3.5" + slot-6 Disk II with
// a 5.25" image), stepping the real apple2cp.rom firmware.
//
// Written for the 2026-07 //c+ dual-controller investigation. The first
// bug it exposed was NOT the dual controller at all: the firmware's boot
// drive-scan polls the IWM status register with devsel=0 (no drive
// enabled), and POM2 answered with the 5.25" image's write-protect bit
// (writable disk → 0) instead of MAME's "no floppy → SENSE high"
// (`iwm.cpp:129`), so the scan waited at $F0FC/$F0FF forever — no banner,
// no boot, on every //c+ cold start.
//
// Usage: iicplus_boot_probe [instructions-in-millions] [disk.dsk]
// Prints PC + slot-6 track + text page every 2M instructions.
// POM2_PROBE_KEYS="SAVE T~CATALOG~" types the string (~ = RETURN) once
// half the instruction budget has elapsed — enough for a DOS 3.3 boot —
// which exercises the 5.25" WRITE path headless.

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "Disk35Image.h"

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

void dumpTextPage(Memory& mem) {
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        char line[41] = {};
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(
                mem.peekMainRam(static_cast<uint16_t>(base + col)) & 0x7F);
            line[col] = (c >= 0x20 && c < 0x7F) ? c : '.';
        }
        std::printf("  |%s|\n", line);
    }
}
}

int main(int argc, char** argv)
{
    const long millions = (argc > 1) ? std::atol(argv[1]) : 30;
    const std::string diskArg = (argc > 2) ? argv[2] : "";

    Memory mem;
    M6502  cpu(&mem);
    pom2::IWMDevice iwm;
    pom2::SmartPortHub hub;
    pom2::Disk35Image img35Int, img35Ext;
    pom2::Sony35Drive sonyInt, sonyExt;
    sonyInt.setImage(&img35Int);
    sonyExt.setImage(&img35Ext);
    hub.attach(&iwm);
    hub.setSony35(&sonyInt, &sonyExt);

    mem.setIWM(&iwm);
    mem.setSmartPortHub(&hub);
    // POM2_PROBE_SHADOW=1 boots with the DiskIICard LSS serving $C0EC
    // reads (the plain-//c arrangement) instead of the IWM walker —
    // the discriminator for the dual-controller investigation.
    const bool shadow = std::getenv("POM2_PROBE_SHADOW") != nullptr;
    mem.setIWMAuthoritative(!shadow);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);

    const std::string rom = firstExisting({"roms/apple2cp.rom"});
    if (rom.empty()) { std::printf("SKIP: no //c+ ROM\n"); return 0; }
    if (!mem.loadAppleIIRom(rom.c_str(), true)) { std::printf("FAIL rom\n"); return 1; }

    auto d2 = std::make_unique<DiskIICard>(6);
    const std::string dr = firstExisting({"roms/disk2.rom"});    if (!dr.empty()) d2->loadBootRom(dr);
    const std::string lr = firstExisting({"roms/diskii_p6.rom"}); if (!lr.empty()) d2->loadLssRom(lr);
    const std::string d5 = !diskArg.empty()
        ? diskArg
        : firstExisting({"disks_5.4/gist/PrintShop.dsk",
                         "disks_5.4/dsk/ProDOS_2_4_3.po"});
    if (!d5.empty()) d2->insertDisk(d5);
    d2->setWriteBackEnabled(true);   // POM2_PROBE_KEYS write tests need it
    DiskIICard* d2raw = d2.get();
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    std::printf("//c+ probe: rom=%s disk=%s budget=%ldM instr\n",
                rom.c_str(), d5.empty() ? "(none)" : d5.c_str(), millions);

    const char* keys = std::getenv("POM2_PROBE_KEYS");
    const long total = millions * 1'000'000L;
    for (long i = 1; i <= total; ++i) {
        cpu.step();
        if (keys && i == total / 2) {
            // Paste FIFO, not queueKey: the latter models the hardware
            // latch (newest key wins), which would drop all but the last
            // character of a scripted burst.
            std::string script(keys);
            for (char& c : script) if (c == '~') c = '\r';
            mem.pasteText(script);
            std::printf("(typed \"%s\" at %ldM)\n", keys, i / 1'000'000);
        }
        if (i % 2'000'000 == 0) {
            std::printf("%3ldM: PC=$%04X trk=%d\n", i / 1'000'000,
                        cpu.getProgramCounter(), d2raw->getTrackPosition(0));
        }
    }

    size_t hgrInk = 0;
    for (int a = 0x2000; a < 0x4000; ++a)
        if (mem.peekMainRam(static_cast<uint16_t>(a)) != 0) ++hgrInk;
    std::printf("final PC=$%04X trk=%d hgrNonZero=%zu — text page:\n",
                cpu.getProgramCounter(), d2raw->getTrackPosition(0), hgrInk);
    dumpTextPage(mem);
    return 0;
}

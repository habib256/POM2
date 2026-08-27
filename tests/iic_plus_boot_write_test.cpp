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

// //c+ cold boot + 5.25" write round-trip — pins the three 2026-07 fixes
// that together took the //c+ from "hangs at $F0FC with no banner" to a
// working DOS 3.3 SAVE/LOAD/RUN:
//
//   1. IWM status with no selected drive reads SENSE HIGH (MAME
//      `iwm.cpp:129` `(!m_floppy || m_floppy->wpt_r()) ? 0x80 : 0`).
//      POM2 answered with the 5.25" image's write-protect bit even at
//      devsel=0, so the firmware's boot drive-scan waited forever.
//   2. Sony 3.5" DSKCHG latch, MAME polarity (`floppy.cpp:560/672/723`,
//      mac wpt_r `!m_dskchg`): an EMPTY drive must sense "changed/empty"
//      (1), not "disk in place" (0) — the scan walked into reading a
//      diskless drive. DIR init is 0 (`floppy.cpp:290`).
//   3. $C0Ex reads are IWM-authoritative ONLY while the hub routes to a
//      3.5" Sony; the 5.25" data path stays on DiskIICard's LSS. The
//      IWM walker mis-framed RWTS enough that write-verify failed
//      (SAVE → I/O ERROR), and its flux write-back double-wrote every
//      sector the LSS also wrote.
//
// Layout: unit checks on Sony35Drive sense polarities (no assets), then
// the full-machine integration boot (skipped when the //c+ ROM or the
// DOS 3.3 master image is absent).

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "Disk35Image.h"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

std::string scrapeTextPage(Memory& mem) {
    std::string out;
    for (int row = 0; row < 24; ++row) {
        const int base = 0x0400 + 0x80 * (row % 8) + 0x28 * (row / 8);
        for (int col = 0; col < 40; ++col) {
            const char c = static_cast<char>(
                mem.peekMainRam(static_cast<uint16_t>(base + col)) & 0x7F);
            out.push_back((c >= 0x20 && c < 0x7F) ? c : ' ');
        }
        out.push_back('\n');
    }
    return out;
}

/// Drive the phase lines so `regSelect()` addresses `reg` (CA0-CA2; the
/// probe never needs HDSEL here), without strobing.
void selectReg(pom2::Sony35Drive& d, uint8_t reg) {
    d.seekPhaseW(static_cast<uint8_t>(reg & 0x07), 0);
}

void testSonySensePolarities()
{
    pom2::Disk35Image img;         // empty slot — no media
    pom2::Sony35Drive drive;
    drive.setImage(&img);
    drive.reset();

    // MAME floppy.cpp:290 — m_dir starts 0.
    selectReg(drive, 0x0);
    assert(drive.senseR() == false);

    // Empty drive: reg 3 (!m_dskchg) reads HIGH (changed/empty) and
    // reg 8 ("no disk in place") reads HIGH. This pair is what tells the
    // //c+ boot scan to skip the drive.
    selectReg(drive, 0x3);
    assert(drive.senseR() == true);

    // DskchgClear strobe (write reg 0xC = HDSEL + CA2) latches
    // m_dskchg = 1 (MAME mac strobe case 0xc), so reg 3 sense drops LOW
    // even with no disk...
    drive.ssW(true);                    // HDSEL → register bank 8-15
    drive.seekPhaseW(0x04, 0);          // CA2 selected, LSTRB low
    drive.seekPhaseW(0x04 | 0x08, 0);   // LSTRB rising → strobe reg 0xC
    drive.ssW(false);
    selectReg(drive, 0x3);
    assert(drive.senseR() == false);

    // ...until the next media event re-derives it (still empty → HIGH,
    // MAME unload() m_dskchg = 0).
    drive.notifyMediaChange();
    selectReg(drive, 0x3);
    assert(drive.senseR() == true);

    std::printf("  ok: Sony sense polarities (DIR init 0, empty drive "
                "reads DSKCHG high)\n");
}

void testIwmSenseWithNoSelectedDrive()
{
    // MAME iwm.cpp:129 — with no selected floppy, the status register's
    // bit 7 (SENSE) reads HIGH regardless of any attached 5.25" image.
    // Attach a WRITABLE image (WP bit low) so the assertion actually
    // discriminates: the old code consulted it at devsel=0 and returned
    // 0 forever — the $F0FC boot hang.
    pom2::IWMDevice iwm;
    iwm.reset();
    DiskImage img;
    img.setWriteBackEnabled(true);      // writable → WP would read 0
    iwm.setFloppy(&img, 0);
    // Q6 on, Q7 off → status read mode; no drive enabled (devsel 0).
    iwm.write(0x0D, 0);   // Q6H
    iwm.read(0x0E);       // Q7L
    const uint8_t st = iwm.read(0x0E);
    assert((st & 0x80) != 0);
    std::printf("  ok: IWM status bit7 high with no selected drive\n");
}

void testIicPlusBootAndWrite()
{
    const std::string rom = firstExisting({"roms/apple2cp.rom"});
    const std::string dsk = firstExisting({"disks_5.4/dsk/dos33_master.dsk"});
    if (rom.empty() || dsk.empty()) {
        std::printf("  SKIP: //c+ ROM or DOS 3.3 master not present\n");
        return;
    }

    // Writable scratch copy — the test SAVEs onto it.
    namespace fs = std::filesystem;
    const fs::path scratch = fs::temp_directory_path() / "pom2_iicplus_wr.dsk";
    fs::copy_file(dsk, scratch, fs::copy_options::overwrite_existing);
    fs::permissions(scratch, fs::perms::owner_write, fs::perm_options::add);

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
    mem.setIWMAuthoritative(true);      // the GUI default
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    if (!mem.loadAppleIIRom(rom.c_str(), true)) {
        std::printf("  SKIP: //c+ ROM failed to load\n");
        return;
    }

    auto d2 = std::make_unique<DiskIICard>(6);
    const std::string dr = firstExisting({"roms/disk2.rom"});
    const std::string lr = firstExisting({"roms/diskii_p6.rom"});
    if (!dr.empty()) d2->loadBootRom(dr);
    if (!lr.empty()) d2->loadLssRom(lr);
    if (!d2->insertDisk(scratch.string())) {
        std::printf("  SKIP: could not insert scratch disk\n");
        return;
    }
    d2->setWriteBackEnabled(true);
    d2->setIWM(&iwm);
    mem.slotBus().plug(6, std::move(d2));

    cpu.setCpuMode(M6502::CpuMode::CMOS);
    cpu.hardReset();

    // Phase 1: cold boot to the DOS 3.3 banner + ] prompt. Before the
    // SENSE fixes the firmware never left $F0FC (blank screen, track 0).
    constexpr long kBootInstr = 40'000'000;
    for (long i = 0; i < kBootInstr; ++i) cpu.step();
    std::string page = scrapeTextPage(mem);
    assert(page.find("DOS VERSION 3.3") != std::string::npos);

    // Phase 2: write round-trip through the //c+ $C0Ex routing. Before
    // the routing fix SAVE ended in I/O ERROR (IWM walker mis-framed the
    // RWTS verify) and the IWM double-wrote the LSS's flux.
    mem.pasteText("NEW\r10 PRINT \"POM2 WRITE OK\"\r"
                  "SAVE POMTEST\rNEW\rLOAD POMTEST\rRUN\r");
    constexpr long kRunInstr = 40'000'000;
    for (long i = 0; i < kRunInstr; ++i) cpu.step();

    page = scrapeTextPage(mem);
    assert(page.find("POM2 WRITE OK") != std::string::npos);
    assert(page.find("I/O ERROR")       == std::string::npos);
    assert(page.find("WRITE PROTECTED") == std::string::npos);

    std::error_code ec;
    fs::remove(scratch, ec);
    std::printf("  ok: //c+ cold boot + DOS 3.3 SAVE/LOAD/RUN round-trip\n");
}

}  // namespace

int main()
{
    std::printf("//c+ boot + 5.25\" write test\n");
    testSonySensePolarities();
    testIwmSenseWithNoSelectedDrive();
    testIicPlusBootAndWrite();
    std::printf("PASS\n");
    return 0;
}

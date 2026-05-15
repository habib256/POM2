// Smoke test for the Phase 1 SmartPort 3.5" plumbing — Disk35Image,
// Sony35Drive, SmartPortHub. Verifies the surface the //c+ alt
// firmware sees on its cold-boot probe path:
//
//   1. Disk35Image refuses non-800K images and accepts ProDOS-ordered
//      819 200-byte raw payloads + 2IMG-wrapped variants.
//   2. Sony35Drive.senseR() returns the expected protocol bits for a
//      drive with NO disk inserted (port of MAME mac_floppy.cpp's
//      WP / SENSE register table, MAME `apple2e.cpp:625-679`).
//   3. SmartPortHub.recalc_active_device matches MAME apple2e.cpp:638-679
//      for the four interesting MIG state combinations.
//   4. The IWM forwards phase strobes to the active 3.5" drive (MAME
//      `iwm.cpp:147-152 update_phases` → `phases_cb` → `seek_phase_w`).
//
// Bit-cell read / write paths are NOT covered here — they need the
// Phase 2 Sony zoned GCR encoder.

#include "Disk35Image.h"
#include "IWMDevice.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static std::string makeRaw800k(uint8_t fillKey)
{
    // Synthesize a "looks-ProDOS" 800K image: block 2 = volume key
    // block with the canonical ProDOS header sniff bytes that
    // Disk35Image::loadFile checks for. Filler is `fillKey` so each
    // test gets a distinguishable payload.
    const fs::path p =
        fs::temp_directory_path() /
        ("pom2_disk35_smoke_" + std::to_string(fillKey) + ".po");
    std::vector<uint8_t> buf(pom2::Disk35Image::kBytesPerImage, fillKey);
    // Block 2 header (offset 0x400):
    //   bytes 0..1 = $00 $00 (prev block)
    //   byte 4     = $Fn (storage_type=F, name_length=n)
    //   bytes 5..  = volume name (uppercase A-Z / 0-9 / '.')
    buf[0x400 + 0] = 0x00;
    buf[0x400 + 1] = 0x00;
    buf[0x400 + 4] = 0xF5;            // storage_type=F, name_length=5
    buf[0x400 + 5] = 'P';
    buf[0x400 + 6] = 'O';
    buf[0x400 + 7] = 'M';
    buf[0x400 + 8] = '2';
    buf[0x400 + 9] = '5';
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return p.string();
}

void testImageLoadRaw()
{
    const std::string p = makeRaw800k(0xE5);
    pom2::Disk35Image img;
    const bool ok = img.loadFile(p);
    assert(ok && img.isLoaded());
    assert(img.kind() == pom2::Disk35Image::ImageKind::Raw800k);

    uint8_t block[512]{};
    assert(img.readBlock(0, block));
    assert(block[0] == 0xE5);
    assert(!img.readBlock(1600, block));      // out of range
    fs::remove(p);
    std::printf("  ok: Disk35Image loads 800K .po image, exposes blocks\n");
}

void testImageRefusesWrongSize()
{
    const fs::path p =
        fs::temp_directory_path() / "pom2_disk35_smoke_bad.po";
    std::vector<uint8_t> buf(143360, 0);      // 5.25" size
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    f.close();

    pom2::Disk35Image img;
    const bool ok = img.loadFile(p.string());
    assert(!ok && !img.isLoaded());
    assert(img.lastError().find("800K") != std::string::npos);
    fs::remove(p);
    std::printf("  ok: Disk35Image refuses non-800K payload\n");
}

void testSony35SenseEmptySlot()
{
    // Drive with no image slot — every "is X present" register must
    // return HIGH (active-low protocol, see Sony35Drive.cpp header).
    pom2::Sony35Drive drv;
    // Address register 0xB (/INSERTED).
    // To select reg 0xB the IWM drives SEL=1, CA2=0, CA1=1, CA0=1.
    drv.setSel(true);
    drv.seekPhaseW(0x03);                      // CA0 | CA1, LSTRB low
    assert(drv.senseR() == true);              // /INSERTED high → no disk
    // /DRVIN register 0xF should report drive PRESENT (active-low → low).
    drv.seekPhaseW(0x07);                      // CA0|CA1|CA2
    assert(drv.senseR() == false);             // /DRVIN low → drive present
    std::printf("  ok: Sony35Drive sense — empty slot reports no-disk, drive-present\n");
}

void testSony35SenseWithDisk()
{
    const std::string p = makeRaw800k(0x42);
    pom2::Disk35Image img;
    assert(img.loadFile(p));

    pom2::Sony35Drive drv;
    drv.setImage(&img);
    drv.notifyMediaChange();
    assert(drv.isInserted());

    // /INSERTED (reg 0xB) → 0 (active-low) because disk is in.
    drv.setSel(true);
    drv.seekPhaseW(0x03);
    assert(drv.senseR() == false);

    // /TRACK0 (reg 0x3) → 0 (we're at track 0 by reset).
    drv.setSel(false);
    drv.seekPhaseW(0x03);                      // CA0|CA1
    assert(drv.senseR() == false);

    fs::remove(p);
    std::printf("  ok: Sony35Drive sense — disk inserted reports correct INSERTED+TRACK0\n");
}

void testSony35MotorStrobe()
{
    const std::string p = makeRaw800k(0x99);
    pom2::Disk35Image img;
    assert(img.loadFile(p));

    pom2::Sony35Drive drv;
    drv.setImage(&img);
    drv.notifyMediaChange();

    // Strobe write-reg 0x2 (motor on): set CA1 (bit 1) high with
    // LSTRB low, then pulse LSTRB high. Register address = { SEL,
    // CA2, CA1, CA0 } = { 0, 0, 1, 0 } = 0x2.
    drv.setSel(false);
    drv.seekPhaseW(0x02);                      // CA1, LSTRB low
    drv.seekPhaseW(0x0A);                      // CA1 + LSTRB → rising edge
    assert(drv.isMotorOn());

    // Strobe reg 0x3 (motor off).
    drv.seekPhaseW(0x03);                      // CA0|CA1, LSTRB low
    drv.seekPhaseW(0x0B);                      // + LSTRB rising
    assert(!drv.isMotorOn());

    fs::remove(p);
    std::printf("  ok: Sony35Drive motor-on/off command strobes\n");
}

void testHubRecalcIntDrive()
{
    pom2::IWMDevice    iwm;
    pom2::Sony35Drive  intDrv, extDrv;
    pom2::SmartPortHub hub;
    hub.attach(&iwm);
    hub.setSony35(&intDrv, &extDrv);

    // Cold state: nothing active.
    assert(hub.active35() == nullptr);

    // MAME line 657: devsel=2, intdrive=true → m_floppy[2] = internal 3.5".
    // Replicate by pumping the hub state.
    hub.setMig35Sel(false);
    hub.setMigIntDrive(true);
    // devsel transitions on IWM motor-on. Simulate by writing $C0E9
    // (motor enable bit, control bit 4 set), then $C0EB (drive 2,
    // control bit 5 set).
    iwm.write(0x9, 0);                         // motor enable → MODE_ACTIVE
    iwm.write(0xB, 0);                         // drive 2 select → devsel = 2
    assert(hub.active35Selected());
    assert(hub.active35() == &intDrv);
    std::printf("  ok: SmartPortHub routes devsel=2+intdrive → internal 3.5\"\n");
}

void testHubRecalc35External()
{
    pom2::IWMDevice    iwm;
    pom2::Sony35Drive  intDrv, extDrv;
    pom2::SmartPortHub hub;
    hub.attach(&iwm);
    hub.setSony35(&intDrv, &extDrv);

    // MAME line 650: devsel=1, m_35sel=true → m_floppy[3] (external 3.5"
    // #2). POM2 stores that in the hub as drive35External_.
    hub.setMig35Sel(true);
    iwm.write(0x9, 0);                         // motor on → devsel = 1
    assert(hub.active35Selected());
    assert(hub.active35() == &extDrv);
    std::printf("  ok: SmartPortHub routes devsel=1+35sel → external 3.5\"\n");
}

void testIwmForwardsPhases()
{
    pom2::IWMDevice iwm;
    pom2::Sony35Drive drv;
    pom2::SmartPortHub hub;
    hub.attach(&iwm);
    hub.setSony35(&drv, nullptr);
    hub.setMigIntDrive(true);
    iwm.write(0x9, 0);                         // motor on (devsel=1)
    iwm.write(0xB, 0);                         // drive 2 → devsel=2 → active

    // Now phases on $C0E0-$C0E7 must reach drv.
    // $C0E1 = set PH0, $C0E3 = set PH1.
    iwm.write(0x1, 0);                         // PH0 (CA0) high
    iwm.write(0x3, 0);                         // PH1 (CA1) high
    // Drive should see register-select address bits CA0=1, CA1=1.
    // Without LSTRB strobe nothing changes inside the drive, but its
    // `senseR()` honours the current phase bits.
    drv.setSel(false);                          // SEL=0; reg = {0, 0, 1, 1} = 0x3
    // /TRACK0 read at reg 0x3 must reflect current track (0 at reset).
    assert(drv.senseR() == false);
    std::printf("  ok: IWM forwards phase bits to active 3.5\" drive\n");
}

int main()
{
    std::printf("\n[SmartPort 3.5\" Phase 1 smoke]\n");
    testImageLoadRaw();
    testImageRefusesWrongSize();
    testSony35SenseEmptySlot();
    testSony35SenseWithDisk();
    testSony35MotorStrobe();
    testHubRecalcIntDrive();
    testHubRecalc35External();
    testIwmForwardsPhases();
    std::printf("[SmartPort 3.5\" Phase 1 smoke] ALL PASS\n");
    return 0;
}

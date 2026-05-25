// Pinned smoke test for the //c-class on-board SmartPort boot path.
//
// Real //c/+/c+ hardware masks ALL slot ROM behind a forced INTCXROM, so a
// plugged ProDOS block card ($Cn00 firmware) is invisible and cannot boot.
// The one exception POM2 carves out: the built-in SmartPort at slot 5, whose
// $C500 firmware is punched through the mask so ProDOS / bootFromSlot(5) see
// a bootable block device. The IWM/Sony GCR 3.5" boot path the real firmware
// uses is unmodelled — POM2 substitutes a host-serviced block stub (the same
// SmartPortCard the //e profile uses). See project_iic_smartport_boot.
//
// This pins:
//   1. SmartPortCard::exposesIicOnboardRom() — false when empty, true once a
//      unit holds media (so the //c autostart never boots an empty card).
//   2. Memory punches the $C500-$C5FF hole on //c-class ONLY when the slot-5
//      card exposes its ROM: the SmartPort signature + driver-entry bytes are
//      visible with media, and the internal ROM returns once media is ejected.
//   3. The block-transfer protocol ($C0D0-$C0D4 device-select, never masked)
//      streams block 0 of the mounted image through Memory end-to-end.

#include "Memory.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

std::string firstExisting(const std::vector<std::string>& candidates)
{
    for (const auto& p : candidates) {
        if (fs::exists(p)) return p;
        if (fs::exists("../" + p))    return "../" + p;
        if (fs::exists("../../" + p)) return "../../" + p;
    }
    return {};
}

// Synthesize a "looks-ProDOS" 800K .po image: block 2 carries the volume-key
// header bytes Disk35Image::loadFile sniffs; block 0 is a uniform fill so the
// read-protocol check is trivial. Mirrors smartport_35_smoke_test.cpp.
std::string makeRaw800k(uint8_t fillKey)
{
    const fs::path p = fs::temp_directory_path() /
        ("pom2_iic_sp_" + std::to_string(fillKey) + ".po");
    std::vector<uint8_t> buf(819200, fillKey);
    buf[0x400 + 0] = 0x00;
    buf[0x400 + 1] = 0x00;
    buf[0x400 + 4] = 0xF5;            // storage_type=F, name_length=5
    buf[0x400 + 5] = 'P'; buf[0x400 + 6] = 'O'; buf[0x400 + 7] = 'M';
    buf[0x400 + 8] = '2'; buf[0x400 + 9] = '5';
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    return p.string();
}

} // namespace

static void testExposesRomOnlyWithMedia()
{
    pom2::SmartPortCard card(5);
    assert(!card.exposesIicOnboardRom());        // empty → masked

    const std::string img = makeRaw800k(0xE5);
    card.setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    std::string err;
    assert(card.mountBay(0, img, err));          // media in → exposed
    assert(card.exposesIicOnboardRom());

    card.ejectBay(0);                            // ejected → masked again
    assert(!card.exposesIicOnboardRom());
    fs::remove(img);
    std::printf("  ok: exposesIicOnboardRom() tracks media presence\n");
}

static void testMemoryHolePunch()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    // No media yet → $C500 hole stays closed (internal ROM). The internal
    // //c SmartPort firmware also starts with a signature, so compare against
    // the *card* bytes specifically: the host stub begins with JMP ($4C) and
    // carries driver-entry $50 at $C5FF — the real firmware does not.
    const uint8_t intC500 = mem.memRead(0xC500);
    const uint8_t intC5FF = mem.memRead(0xC5FF);
    assert(!(intC500 == 0x4C && intC5FF == 0x50));   // hole closed

    // Arm + mount media → hole opens, card firmware visible. (The hole is
    // gated on the SmartPort being "armed" by an explicit boot — see
    // Memory::setIicSmartPortArmed; bootFromSlot sets it at runtime.)
    const std::string img = makeRaw800k(0x33);
    std::string err;
    raw->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    assert(raw->mountBay(0, img, err));

    // Still closed until armed, even with media present.
    assert(!(mem.memRead(0xC500) == 0x4C && mem.memRead(0xC5FF) == 0x50));
    mem.setIicSmartPortArmed(true);

    assert(mem.memRead(0xC500) == 0x4C);   // JMP $C520 (boot trampoline)
    assert(mem.memRead(0xC501) == 0x20);   // ProDOS signature
    assert(mem.memRead(0xC503) == 0x00);
    assert(mem.memRead(0xC505) == 0x03);
    assert(mem.memRead(0xC507) == 0x01);   // ProDOS block-device class
    assert(mem.memRead(0xC5FF) == 0x50);   // driver entry $C550

    // Eject → hole closes again.
    raw->ejectBay(0);
    assert(!(mem.memRead(0xC500) == 0x4C && mem.memRead(0xC5FF) == 0x50));

    fs::remove(img);
    std::printf("  ok: Memory punches $C500 hole only when slot-5 card has media\n");
}

static void testBlockReadThroughMemory()
{
    const std::string rom = firstExisting({
        "roms/apple2c-32Kv0.rom", "roms/apple2cp.rom",
    });
    if (rom.empty()) {
        std::printf("  SKIP: no 32 KB //c-class ROM present\n");
        return;
    }

    Memory mem;
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.setIIEMode(true);
    assert(mem.loadAppleIIRom(rom.c_str(), /*pickLowerHalf=*/true));

    auto card = std::make_unique<pom2::SmartPortCard>(5);
    pom2::SmartPortCard* raw = card.get();
    mem.slotBus().plug(5, std::move(card));

    const std::string img = makeRaw800k(0xC7);
    std::string err;
    raw->setUnit(0, std::make_unique<pom2::SmartPort35Unit>());
    assert(raw->mountBay(0, img, err));

    // Device-select I/O ($C0D0-$C0DF = slot 5) is never masked on //c-class:
    // select unit 0, point at block 0, stream 512 bytes back through Memory.
    mem.memWrite(0xC0D0, 0x00);   // unit select 0
    mem.memWrite(0xC0D1, 0x00);   // block LO
    mem.memWrite(0xC0D2, 0x00);   // block HI
    for (int i = 0; i < 512; ++i) {
        const uint8_t b = mem.memRead(0xC0D3);
        assert(b == 0xC7);        // block 0 is a uniform fill
    }
    const uint8_t status = mem.memRead(0xC0D4);
    assert((status & 0x01) == 0); // no I/O error
    assert((status & 0x80) == 0); // media present

    fs::remove(img);
    std::printf("  ok: block 0 streams through Memory $C0D3 (slot-5 device-select)\n");
}

int main()
{
    std::printf("\n[//c on-board SmartPort smoke]\n");
    testExposesRomOnlyWithMedia();
    testMemoryHolePunch();
    testBlockReadThroughMemory();
    std::printf("[//c on-board SmartPort smoke] ALL PASS\n");
    return 0;
}

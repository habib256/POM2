// GrapplerCard smoke test — pins:
//
//   1. Fallback stub ROM (no Grappler+ dump): exposes the synthetic PR#n
//      trampoline + Pascal autodetect bytes so `PR#1` still works without
//      the 4 KB Grappler dump.
//   2. Data port spool — writes to $C0(8+s)1 enqueue bytes verbatim; reads
//      return $FF; spool clears on demand.
//   3. ROM gate — `isRomLoaded()` is false until `loadRom()` succeeds; a
//      wrong-size dump is rejected and the stub stays in place.

#include "GrapplerCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

void testStubRom()
{
    GrapplerCard card(1);
    assert(!card.isRomLoaded());

    // PR#n entry at $Cn00 — JMP $Cn20.
    assert(card.slotRomRead(0x00) == 0x4C);
    assert(card.slotRomRead(0x01) == 0x20);
    assert(card.slotRomRead(0x02) == 0xC1);     // slotHi for slot 1
    // Pascal 1.1 autodetect.
    assert(card.slotRomRead(0x05) == 0x38);
    assert(card.slotRomRead(0x07) == 0x18);
    assert(card.slotRomRead(0x0B) == 0x01);
    assert(card.slotRomRead(0x0C) == 0x00);
    // CSWL/CSWH installer.
    assert(card.slotRomRead(0x20) == 0xA9);
    assert(card.slotRomRead(0x21) == 0x31);
    assert(card.slotRomRead(0x25) == 0xC1);
    // Output handler.
    assert(card.slotRomRead(0x31) == 0x8D);
    assert(card.slotRomRead(0x32) == 0x91);     // $80 + 1*16 + 1
    assert(card.slotRomRead(0x33) == 0xC0);
    assert(card.slotRomRead(0x34) == 0x60);

    // Slot 3 rebakes slot-dependent bytes.
    GrapplerCard card3(3);
    assert(card3.slotRomRead(0x02) == 0xC3);
    assert(card3.slotRomRead(0x25) == 0xC3);
    assert(card3.slotRomRead(0x32) == 0xB1);    // $80 + 3*16 + 1

    // Expansion ROM is open bus while the stub is active.
    assert(card.expansionRomRead(0x000) == 0xFF);
    assert(card.expansionRomRead(0x100) == 0xFF);

    std::printf("  ok: stub ROM fingerprint\n");
}

void testDataPortSpool()
{
    GrapplerCard card(1);

    for (uint8_t i = 0; i < 16; ++i)
        assert(card.deviceSelectRead(i) == 0xFF);

    card.deviceSelectWrite(1, 0xC8);
    card.deviceSelectWrite(1, 0xC9);
    card.deviceSelectWrite(1, 0x8D);
    assert(card.bytesWritten() == 3);
    const auto raw = card.spoolBytes();
    assert(raw[0] == 0xC8 && raw[1] == 0xC9 && raw[2] == 0x8D);
    assert(card.spoolText() == "HI\n");

    // Non-data offsets ignored.
    card.deviceSelectWrite(0, 0xFF);
    card.deviceSelectWrite(2, 0xFF);
    assert(card.bytesWritten() == 3);

    card.clearSpool();
    assert(card.bytesWritten() == 0);

    std::printf("  ok: data port + spool semantics\n");
}

void testRomLoadGate()
{
    GrapplerCard card(1);

    // Missing file is rejected, stub stays in place.
    assert(!card.loadRom("/this/path/does/not/exist.bin"));
    assert(!card.isRomLoaded());

    // Wrong-size payload is also rejected so a truncated dump doesn't
    // silently break software detection.
    const std::string tmp = "/tmp/pom2_grappler_bad.bin";
    {
        std::ofstream f(tmp, std::ios::binary);
        // Anything that isn't exactly 4096 bytes.
        for (int i = 0; i < 1024; ++i) f.put(static_cast<char>(i & 0xFF));
    }
    assert(!card.loadRom(tmp));
    assert(!card.isRomLoaded());
    std::remove(tmp.c_str());

    // A 4 KB blob loads cleanly.
    const std::string good = "/tmp/pom2_grappler_good.bin";
    {
        std::ofstream f(good, std::ios::binary);
        for (int i = 0; i < 4096; ++i) f.put(static_cast<char>(i & 0xFF));
    }
    assert(card.loadRom(good));
    assert(card.isRomLoaded());
    // Slot ROM now mirrors the file bytes.
    assert(card.slotRomRead(0x00) == 0x00);
    assert(card.slotRomRead(0x10) == 0x10);
    // Expansion ROM is the first 2 KB.
    assert(card.expansionRomRead(0x000) == 0x00);
    assert(card.expansionRomRead(0x100) == 0x00);    // 0x100 & 0xFF
    std::remove(good.c_str());

    std::printf("  ok: ROM-load size gate\n");
}

} // namespace

int main()
{
    std::printf("GrapplerCard smoke test\n");
    testStubRom();
    testDataPortSpool();
    testRomLoadGate();
    std::printf("PASS\n");
    return 0;
}

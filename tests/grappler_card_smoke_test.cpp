// GrapplerCard smoke test — pins:
//
//   1. Fallback stub ROM (no Grappler+ dump): exposes the synthetic PR#n
//      trampoline + Pascal autodetect bytes so `PR#1` still works without
//      the 4 KB Grappler dump.
//   2. MAME-faithful $C0nX decode (a2bus_grapplerplus, grappler.cpp):
//      data latch on !(offset & 3) ($0/$4/$8/$C → spool), A0 = high ROM
//      bank at $C800, A1/A2 = IRQ disable/enable; status = IRQ|BUSY|PE|
//      SELECT|ACK with the synthetic printer always ready ($03 idle).
//      (The previous decode spooled offset 1 — the real card's BANK
//      SELECT — so genuine firmware printed into the void and read $FF
//      status = busy + paper out.)
//   3. ROM gate — `isRomLoaded()` is false until `loadRom()` succeeds; a
//      wrong-size dump is rejected and the stub stays in place; $CnXX
//      reads reset the expansion bank per MAME read_cnxx.

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
    // Output handler — data port is $C0(8+s)0, the MAME data-latch offset.
    assert(card.slotRomRead(0x31) == 0x8D);
    assert(card.slotRomRead(0x32) == 0x90);     // $80 + 1*16 + 0
    assert(card.slotRomRead(0x33) == 0xC0);
    assert(card.slotRomRead(0x34) == 0x60);

    // Slot 3 rebakes slot-dependent bytes.
    GrapplerCard card3(3);
    assert(card3.slotRomRead(0x02) == 0xC3);
    assert(card3.slotRomRead(0x25) == 0xC3);
    assert(card3.slotRomRead(0x32) == 0xB0);    // $80 + 3*16 + 0

    // Expansion ROM is open bus while the stub is active.
    assert(card.expansionRomRead(0x000) == 0xFF);
    assert(card.expansionRomRead(0x100) == 0xFF);

    std::printf("  ok: stub ROM fingerprint\n");
}

void testDataPortSpool()
{
    GrapplerCard card(1);

    // Idle status on every offset: no IRQ (bit 7), DIP 000 (6-4),
    // BUSY=0 (3), PE=0 (2), SELECT=1 (1), ACK latch set (0) → $03.
    for (uint8_t i = 0; i < 16; ++i)
        assert(card.deviceSelectRead(i) == 0x03);

    // Data latch decodes !(offset & 3): $0/$4/$8/$C all spool.
    card.deviceSelectWrite(0x0, 0xC8);
    card.deviceSelectWrite(0x4, 0xC9);
    card.deviceSelectWrite(0x8, 0x8D);
    assert(card.bytesWritten() == 3);
    const auto raw = card.spoolBytes();
    assert(raw[0] == 0xC8 && raw[1] == 0xC9 && raw[2] == 0x8D);
    assert(card.spoolText() == "HI\n");
    card.deviceSelectWrite(0xC, 0x21);
    assert(card.bytesWritten() == 4);

    // Non-data offsets do NOT spool: 1 = bank select, 2 = IRQ disable,
    // 3 = bank + disable, 5 = bank + enable…
    card.deviceSelectWrite(0x1, 0xFF);
    card.deviceSelectWrite(0x2, 0xFF);
    card.deviceSelectWrite(0x3, 0xFF);
    assert(card.bytesWritten() == 4);

    // IRQ enable (A2): with the instant-ACK latch set, the pending bit
    // appears in bit 7; A1 disables and releases it. (No bus attached —
    // assertIrq is a documented no-op when unplugged.)
    card.deviceSelectWrite(0x4 | 0x0, 0x00);       // data + keep enabled state
    card.deviceSelectWrite(0x5, 0x00);             // A0 bank + A2 enable
    assert((card.deviceSelectRead(0) & 0x80) != 0);
    card.deviceSelectWrite(0x2, 0x00);             // A1 disable
    assert((card.deviceSelectRead(0) & 0x80) == 0);

    // Reset: IRQ disabled, ACK latch back to set, idle status again.
    card.deviceSelectWrite(0x5, 0x00);
    card.onReset();
    assert(card.deviceSelectRead(0) == 0x03);

    card.clearSpool();
    assert(card.bytesWritten() == 0);

    std::printf("  ok: MAME c0nx decode + spool semantics\n");
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

    // A 4 KB blob loads cleanly. Fill = high byte of the address so the
    // two 2 KB expansion banks are distinguishable ($00-$07 low bank,
    // $08-$0F high bank).
    const std::string good = "/tmp/pom2_grappler_good.bin";
    {
        std::ofstream f(good, std::ios::binary);
        for (int i = 0; i < 4096; ++i) f.put(static_cast<char>((i >> 8) & 0xFF));
    }
    assert(card.loadRom(good));
    assert(card.isRomLoaded());
    // Slot ROM now mirrors the file bytes (page 0 of the dump).
    assert(card.slotRomRead(0x00) == 0x00);
    assert(card.slotRomRead(0x10) == 0x00);
    // Expansion ROM starts on the LOW 2 KB bank.
    assert(card.expansionRomRead(0x000) == 0x00);
    assert(card.expansionRomRead(0x100) == 0x01);
    // A0 device-select write flips to the HIGH bank (MAME set_rom_bank).
    card.deviceSelectWrite(0x1, 0x00);
    assert(card.expansionRomRead(0x000) == 0x08);
    assert(card.expansionRomRead(0x700) == 0x0F);
    // Any $CnXX read resets the bank to 0 (MAME read_cnxx side effect).
    (void)card.slotRomRead(0x00);
    assert(card.expansionRomRead(0x000) == 0x00);
    std::remove(good.c_str());

    std::printf("  ok: ROM-load size gate + $C800 banking\n");
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

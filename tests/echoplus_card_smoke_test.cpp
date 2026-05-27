// EchoPlusCard smoke test — verifies the SlotPeripheral surface that
// glues the SSI263 chip model to the slot bus:
//   (1) slotRomRead/Write at $Cs00..$Cs04 reach the 5 SSI263 registers;
//   (2) $Cs05..$CsFF read as open bus ($FF);
//   (3) advanceCycles ticks the chip and asserts the slot IRQ when
//       A/!R goes high;
//   (4) ack via a write to $00..$02 releases the slot IRQ.
//
// Chip-level state-machine semantics are pinned by `ssi263_smoke` —
// this test focuses on the bus glue.

#include "EchoPlusCard.h"
#include "Ssi263.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

void testRegisterDecode()
{
    EchoPlusCard card(4);

    // Write all 5 registers via slotRomWrite.
    card.slotRomWrite(0x0, 0x55);
    card.slotRomWrite(0x1, 0xAA);
    card.slotRomWrite(0x2, 0xF0);   // rate=15
    card.slotRomWrite(0x3, 0x0F);   // CTL=0, amp=15 (exit power-down)
    card.slotRomWrite(0x4, 0x80);

    auto s = card.snapshotChip();
    // DURPHON write triggers a phoneme + bumps the counter (since
    // CTL=0 was set in the same sequence by $03 — order matters here
    // for the v1 chip-write semantics, but writing $00 once with CTL
    // already cleared bumps the count).
    assert(s.regs[0] == 0x55);
    assert(s.regs[1] == 0xAA);
    assert(s.regs[2] == 0xF0);
    assert(s.regs[3] == 0x0F);
    assert(s.regs[4] == 0x80);
    assert(!s.powerDown);
    assert(s.irqEnabled);

    // Out-of-range reads = open bus.
    assert(card.slotRomRead(0x05) == 0xFF);
    assert(card.slotRomRead(0x80) == 0xFF);
    assert(card.slotRomRead(0xFF) == 0xFF);

    // Out-of-range writes are no-ops (no chip state change).
    const auto regsBefore = s.regs;
    card.slotRomWrite(0x05, 0x42);
    card.slotRomWrite(0xC0, 0x42);
    auto s2 = card.snapshotChip();
    for (int r = 0; r < 5; ++r) assert(s2.regs[r] == regsBefore[r]);

    std::printf("  ok: $Cs00-$Cs04 routes to SSI263; rest = open bus\n");
}

void testIrqWiring()
{
    EchoPlusCard card(4);
    // Configure a fast phoneme with IRQ enabled (mode 11).
    card.slotRomWrite(0x3, 0x0F);     // exit power-down
    card.slotRomWrite(0x2, 0xF0);     // rate=15 (fast)
    card.slotRomWrite(0x0, 0xC1);     // mode=11, phoneme=1

    // Slot IRQ is low until the phoneme finishes.
    assert(!card.isIrqAsserted());
    auto s = card.snapshotChip();
    assert(s.phonemeRemainingCycles > 0);

    // Tick past the duration → A/!R should be set + slot IRQ asserted.
    card.advanceCycles(s.phonemeRemainingCycles + 100);
    assert(card.isIrqAsserted());
    s = card.snapshotChip();
    assert(s.aRequest);

    // Slot IRQ stays high until the host CPU acks (writes $00-$02).
    card.advanceCycles(10000);
    assert(card.isIrqAsserted());

    // Ack via $00 (DURPHON) write: A/!R clears, slot IRQ releases,
    // new phoneme starts.
    card.slotRomWrite(0x0, 0xC2);
    assert(!card.isIrqAsserted());
    s = card.snapshotChip();
    assert(!s.aRequest);
    assert(s.currentPhoneme == 2);

    std::printf("  ok: advanceCycles past phoneme → slot IRQ assert; "
                "$00 ack → release\n");
}

void testReadAcknowledgement()
{
    EchoPlusCard card(4);
    // Drive a phoneme to completion.
    card.slotRomWrite(0x3, 0x0F);
    card.slotRomWrite(0x2, 0xF0);
    card.slotRomWrite(0x0, 0xC5);
    auto s = card.snapshotChip();
    card.advanceCycles(s.phonemeRemainingCycles + 100);
    assert(card.isIrqAsserted());

    // Reads return $80 (A/!R bit 7 set). Reads do NOT clear A/!R —
    // only writes to $00-$02 do (per AppleWin SSI263.cpp).
    assert(card.slotRomRead(0x0) == 0x80);
    assert(card.slotRomRead(0x3) == 0x80);
    assert(card.isIrqAsserted());

    // Reset state: chip in power-down, IRQ released.
    card.onReset();
    assert(!card.isIrqAsserted());
    s = card.snapshotChip();
    assert(s.powerDown);
    assert(!s.aRequest);

    std::printf("  ok: reads see A/!R bit but don't ack; onReset clears\n");
}

} // namespace

int main()
{
    std::printf("EchoPlusCard smoke test\n");
    testRegisterDecode();
    testIrqWiring();
    testReadAcknowledgement();
    std::printf("PASS\n");
    return 0;
}

// ClockCard smoke test — pins the ProDOS clock-detection signature and
// the uPD1990AC bit-bang protocol the card emulates so ProDOS's hard-
// coded ThunderClock+ driver can talk to it. The driver lives inside
// ProDOS itself; here we drive the chip from C++ exactly as a host
// would and assert it serialises out the right BCD bytes.

#include "ClockCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>

namespace {

constexpr uint8_t kBitDataIn = 0x01;
constexpr uint8_t kBitClk    = 0x02;
constexpr uint8_t kBitStb    = 0x04;
// Mode bits live in bits 3..5. MODE_TIME_READ = 0b011 → 0x18 raw.
constexpr uint8_t kModeTimeReadShifted = 0x03 << 3;     // 0x18

std::tm fixedTime_2026_05_09_14_37_42()
{
    std::tm t{};
    t.tm_year  = 126;     // 2026 - 1900
    t.tm_mon   = 4;       // May (0-indexed)
    t.tm_mday  = 9;
    t.tm_hour  = 14;
    t.tm_min   = 37;
    t.tm_sec   = 42;
    t.tm_wday  = 6;       // Saturday
    return t;
}

void testSignature()
{
    ClockCard card(4);
    // ProDOS scans for these exact bytes at the listed offsets.
    assert(card.slotRomRead(0x00) == 0x08);     // PHP
    assert(card.slotRomRead(0x02) == 0x28);     // PLP
    assert(card.slotRomRead(0x04) == 0x58);     // CLI
    assert(card.slotRomRead(0x06) == 0x70);     // BVS
    // BVS operand must be 0 so the no-overflow path also lands at $Cs08.
    assert(card.slotRomRead(0x07) == 0x00);
    // Filler bytes between signature ops are 1-byte instructions so the
    // CPU can walk straight through if anything ever JMPs to $Cs00.
    assert(card.slotRomRead(0x01) == 0xD8);     // CLD
    assert(card.slotRomRead(0x03) == 0xD8);     // CLD
    assert(card.slotRomRead(0x05) == 0x78);     // SEI (re-disable IRQ)
    // $Cs08 is RTS — a stray JMP $Cs00 falls through and returns cleanly.
    assert(card.slotRomRead(0x08) == 0x60);
}

// Helper: read one DATA_OUT bit and advance the chip by pulsing CLK.
// The chip's DATA_OUT is "live" (always the current LSB of the shift
// register), so the host samples BEFORE the CLK rising edge. The
// rising edge shifts the register one position right so the *next*
// call sees the next bit. This matches the loop in ProDOS's hardcoded
// ThunderClock driver: `LDA $C080,X ; ASL A ; ROR buf ; <pulse CLK>`.
uint8_t readBitThenAdvance(ClockCard& card, uint8_t baseline)
{
    const uint8_t bit = (card.deviceSelectRead(0) & 0x80) ? 1 : 0;
    card.deviceSelectWrite(0, baseline & ~kBitClk);    // CLK low (idempotent)
    card.deviceSelectWrite(0, baseline |  kBitClk);    // CLK rising → shift
    card.deviceSelectWrite(0, baseline & ~kBitClk);    // CLK back low
    return bit;
}

// Helper: read 8 bits LSB-first from the chip into one BCD byte.
uint8_t readByteLsbFirst(ClockCard& card, uint8_t baseline)
{
    uint8_t out = 0;
    for (int i = 0; i < 8; ++i) {
        const uint8_t bit = readBitThenAdvance(card, baseline);
        out = static_cast<uint8_t>(out | (bit << i));
    }
    return out;
}

// End-to-end of the read path: latch host time via STB+MODE_TIME_READ,
// shift out 6 bytes, and check each one against the fixed timestamp.
void testTimeReadProtocol()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Step 1: arm MODE_TIME_READ on C0/C1/C2 with STB still low.
    //   bits 3..5 = 011 → 0x18; STB = 0; CLK = 0.
    card->deviceSelectWrite(0, kModeTimeReadShifted);

    // Step 2: STB rising edge with mode=TIME_READ snapshots host time
    // into the 48-bit shift register.
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);

    // Step 3: drop STB so subsequent writes only pulse CLK. Mode bits
    // can be left at 0 — the chip latched on STB rise. Keep them at
    // TIME_READ to match what real driver code does.
    const uint8_t baseline = kModeTimeReadShifted;     // STB low, CLK low
    card->deviceSelectWrite(0, baseline);

    // Step 4: clock out 6 bytes LSB-first.
    const uint8_t sec    = readByteLsbFirst(*card, baseline);
    const uint8_t min    = readByteLsbFirst(*card, baseline);
    const uint8_t hour   = readByteLsbFirst(*card, baseline);
    const uint8_t day    = readByteLsbFirst(*card, baseline);
    const uint8_t mthDow = readByteLsbFirst(*card, baseline);
    const uint8_t year   = readByteLsbFirst(*card, baseline);

    // 2026-05-09 14:37:42 Saturday → BCD bytes:
    //   sec=$42, min=$37, hour=$14, day=$09, month=$05, year=$26
    //   shiftReg[4] = (toBcd(5) & 0xF0) | (6 & 0x0F) = $00 | $06 = $06
    //     (Because BCD 5 is $05, with the '5' digit in the LOW nibble —
    //      `toBcd(5) & 0xF0` masks it out, leaving the low-nibble dow.
    //      That mismatches what a real chip expects; see the comment in
    //      ClockCard::latchTimeToShiftReg for why ProDOS doesn't care.)
    assert(sec  == 0x42);
    assert(min  == 0x37);
    assert(hour == 0x14);
    assert(day  == 0x09);
    assert(year == 0x26);
    // The month/dow byte is the place where the chip's quirky packing
    // shows; assert it bit-exactly so changes to the latch routine are
    // forced through this test.
    assert(mthDow == 0x06);
}

// After all bits have been shifted out, further CLK pulses should keep
// reading zeros (the shift register has been emptied) and never hang or
// spin. This pins the safety of an over-long driver loop.
void testShiftDrainsToZero()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
    const uint8_t baseline = kModeTimeReadShifted;
    card->deviceSelectWrite(0, baseline);

    // Shift out all 48 bits.
    for (int i = 0; i < 48; ++i) (void)readBitThenAdvance(*card, baseline);

    // Next 16 bits should all read zero — the register has drained.
    for (int i = 0; i < 16; ++i) {
        assert(readBitThenAdvance(*card, baseline) == 0);
    }
}

// Multiple STB pulses re-snapshot host time. The test injector returns
// the same time each call, so the bytes after a re-snapshot should
// match the bytes from the first snapshot.
void testStbRelatchesTime()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    auto readSeconds = [&]() -> uint8_t {
        card->deviceSelectWrite(0, kModeTimeReadShifted);
        card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
        const uint8_t baseline = kModeTimeReadShifted;
        card->deviceSelectWrite(0, baseline);
        return readByteLsbFirst(*card, baseline);
    };

    assert(readSeconds() == 0x42);
    // Drop STB, re-arm — should still read the same fixed timestamp.
    assert(readSeconds() == 0x42);
}

// Reads outside $C0n0 are open bus — pinned so any future register
// expansion is intentional rather than accidental.
void testOpenBusOnOtherDeviceSelectOffsets()
{
    ClockCard card(4);
    assert(card.deviceSelectRead(1)  == 0xFF);
    assert(card.deviceSelectRead(2)  == 0xFF);
    assert(card.deviceSelectRead(15) == 0xFF);
}

}  // namespace

int main()
{
    testSignature();
    std::printf("ProDOS detection signature: OK\n");

    testTimeReadProtocol();
    std::printf("uPD1990AC time-read protocol round-trip: OK\n");

    testShiftDrainsToZero();
    std::printf("Shift register drains to zero: OK\n");

    testStbRelatchesTime();
    std::printf("STB rising-edge relatches time: OK\n");

    testOpenBusOnOtherDeviceSelectOffsets();
    std::printf("Open bus on non-$C0n0 device selects: OK\n");

    std::printf("clock_card_smoke OK\n");
    return 0;
}

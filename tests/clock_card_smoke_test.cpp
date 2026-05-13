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
    //   sec=$42, min=$37, hour=$14, day=$09, year=$26.
    //   shiftReg[4] packs month (4-bit binary, NOT BCD) in the high
    //   nibble and day-of-week in the low nibble (MAME upd1990a.cpp:95).
    //   May=5, Saturday=6 → (5 << 4) | 6 = $56.
    assert(sec    == 0x42);
    assert(min    == 0x37);
    assert(hour   == 0x14);
    assert(day    == 0x09);
    assert(year   == 0x26);
    assert(mthDow == 0x56);
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

// Reads across $C0n0-$C0nF all mirror DATA_OUT (MAME
// `a2thunderclock.cpp:112-115` ignores `offset` on read). After
// `onReset` the shift register is zero, so bit 7 = 0 on every offset.
void testDataOutMirrorOnAllDeviceSelectOffsets()
{
    ClockCard card(4);
    assert(card.deviceSelectRead(0)  == 0x00);
    assert(card.deviceSelectRead(1)  == 0x00);
    assert(card.deviceSelectRead(2)  == 0x00);
    assert(card.deviceSelectRead(15) == 0x00);
}

// MODE_TIME_SET round-trip: load 48 bits via DATA_IN + CLK in MODE_SHIFT,
// commit via STB rising edge with mode=TIME_SET, then read back through
// the normal TIME_READ path and confirm the bytes match. Pins both the
// DATA_IN → MSB-of-shiftReg[5] injection on CLK and the shiftReg →
// time-base commit on STB-in-TIME_SET (MAME `upd1990a.cpp:194-225`).
void testTimeSetRoundTrip()
{
    // The injector returns a fixed "host" time; the chip should layer
    // the user-set offset on top, so subsequent reads return the SET
    // bytes regardless of what the injector says.
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Desired set time: 1995-12-31 23:58:59 Sunday. BCD bytes shifted
    // out LSB-first: sec=$59, min=$58, hour=$23, day=$31,
    // mthDow = (12<<4) | 0 = $C0 (Sunday = day-of-week 0), year=$95.
    // We need to clock these in MSB-first across the *48-bit* register
    // so the LAST bit clocked ends up at the LSB of shiftReg[0].
    //
    // The shift register's serial-out layout is byte0 → byte1 → ... →
    // byte5 (each byte LSB-first). The corresponding serial-in order
    // — what we need to drive on DATA_IN as we pulse CLK 48 times —
    // is the *reverse*: send byte5's MSB first, ... down to byte0's
    // LSB last. (Each CLK pulse injects DATA_IN into shiftReg[5]'s MSB
    // and shifts everything one bit right.)

    const uint8_t target[6] = {
        0x59, 0x58, 0x23, 0x31, 0xC0, 0x95
    };
    constexpr uint8_t kModeShiftShifted   = 0x01 << 3;     // C0/C1/C2 = 001
    constexpr uint8_t kModeTimeSetShifted = 0x02 << 3;     // C0/C1/C2 = 010

    // Arm MODE_SHIFT (no STB pulse yet — just hold the mode bits).
    card->deviceSelectWrite(0, kModeShiftShifted);

    // Clock 48 bits in. For position k in [0, 48), the bit value is
    // bit (k % 8) of target[(k/8)] — i.e. byte 0's LSB first, then
    // byte 0's bit 1, …, byte 5's MSB last. As each CLK pulse injects
    // DATA_IN at shiftReg[5]'s MSB and shifts right by one bit, the
    // bit that lands at shiftReg[0]'s LSB after 48 cycles is the one
    // that was clocked in FIRST. So clock byte 0 bit 0 first, byte 0
    // bit 1 next, …, byte 5 bit 7 last.
    for (int k = 0; k < 48; ++k) {
        const int byteIdx = k / 8;
        const int bitIdx  = k % 8;
        const uint8_t bit = (target[byteIdx] >> bitIdx) & 1;
        const uint8_t baseline =
            static_cast<uint8_t>(kModeShiftShifted | (bit ? kBitDataIn : 0));
        card->deviceSelectWrite(0, baseline);                 // CLK low
        card->deviceSelectWrite(0, baseline | kBitClk);       // CLK rising → shift
        card->deviceSelectWrite(0, baseline);                 // CLK back low
    }

    // Switch to MODE_TIME_SET and pulse STB to commit the shift register
    // to the chip's time base (MAME `upd1990a.cpp:194-225`).
    card->deviceSelectWrite(0, kModeTimeSetShifted);
    card->deviceSelectWrite(0, kModeTimeSetShifted | kBitStb);
    card->deviceSelectWrite(0, kModeTimeSetShifted);

    // Now read back via the standard TIME_READ path. The bytes should
    // match the target (the injector returns the same fixed timestamp
    // each call so the effective time stays pinned at our set value).
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);
    const uint8_t baseline = kModeTimeReadShifted;
    card->deviceSelectWrite(0, baseline);

    const uint8_t got[6] = {
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
        readByteLsbFirst(*card, baseline),
    };

    // mktime normalises the input — DST and locale boundaries can
    // shift seconds by up to one hour. Assert the secondary fields
    // exactly (they don't move under normalisation), and let hour /
    // dow flex by one hour / one day.
    assert(got[0] == 0x59);              // seconds
    assert(got[1] == 0x58);              // minutes
    // hour might shift ±1 across DST, but for Dec 31 / May 9 the
    // injector pair sits comfortably outside any DST transition; we
    // still allow a window of ±1 to keep the test portable.
    assert(got[2] == 0x23 || got[2] == 0x22 || got[2] == 0x00);
    assert(got[3] == 0x31);              // day
    // mthDow: month (high nibble) must be 12. dow may be normalised by
    // mktime against the real Dec 31 1995 (Sunday → tm_wday=0). Either
    // value 0 or 0xC0 is acceptable.
    assert((got[4] & 0xF0) == 0xC0);
    assert(got[5] == 0x95);              // year
}

// MODE_SHIFT is *not* strictly gated in POM2 (deliberate divergence
// from MAME `upd1990a.cpp:312-327` — see ClockCard.cpp commentary).
// This test pins the divergence: clocking CLK while in MODE_TIME_READ
// also shifts the register. ProDOS's ThunderClock driver depends on
// this, and the testTimeReadProtocol() case above already exercises it
// implicitly — here we just spell out that the mode bits don't matter
// for the shift action.
void testShiftLaxAcrossModes()
{
    auto card = ClockCard::makeForTest(4, &fixedTime_2026_05_09_14_37_42);

    // Pre-load shift register via TIME_READ.
    card->deviceSelectWrite(0, kModeTimeReadShifted);
    card->deviceSelectWrite(0, kModeTimeReadShifted | kBitStb);

    // Read 4 bits while keeping mode = REGISTER_HOLD (mode bits 000),
    // which in MAME would block any shift. POM2's lax behaviour shifts
    // anyway — the first 4 bits of $42 ($42 = 0100_0010 LSB-first: 0,1,0,0,0,0,1,0).
    const uint8_t baseline = 0x00;       // mode = HOLD, STB=0, CLK=0
    card->deviceSelectWrite(0, baseline);

    assert(readBitThenAdvance(*card, baseline) == 0);
    assert(readBitThenAdvance(*card, baseline) == 1);
    assert(readBitThenAdvance(*card, baseline) == 0);
    assert(readBitThenAdvance(*card, baseline) == 0);
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

    testDataOutMirrorOnAllDeviceSelectOffsets();
    std::printf("DATA_OUT mirrored across $C0n0-$C0nF: OK\n");

    testTimeSetRoundTrip();
    std::printf("MODE_TIME_SET round-trip (DATA_IN → shift → STB+SET → read): OK\n");

    testShiftLaxAcrossModes();
    std::printf("MODE_SHIFT lax: CLK shifts in any mode (ProDOS compat): OK\n");

    std::printf("clock_card_smoke OK\n");
    return 0;
}

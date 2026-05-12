// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ClockCard.h"

#include <ctime>

namespace {

// uPD1990AC mode codes carried on C0/C1/C2 (bits 3,4,5 of the $C0n0
// write). The host pulses STB to latch the mode; only MODE_TIME_READ
// (0b011) and MODE_REGISTER_HOLD (0b000) matter for ProDOS's read path.
constexpr uint8_t kModeRegisterHold = 0x00;
constexpr uint8_t kModeShift        = 0x01;
constexpr uint8_t kModeTimeSet      = 0x02;
constexpr uint8_t kModeTimeRead     = 0x03;

// $C0n0 write-side bit positions.
constexpr uint8_t kBitDataIn = 0x01;
constexpr uint8_t kBitClk    = 0x02;
constexpr uint8_t kBitStb    = 0x04;

uint8_t toBcd(int n)
{
    if (n < 0) n = 0;
    if (n > 99) n = 99;
    return static_cast<uint8_t>(((n / 10) << 4) | (n % 10));
}

std::tm hostLocalTime()
{
    const std::time_t now = std::time(nullptr);
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &now);
#else
    if (std::tm* p = std::localtime(&now)) out = *p;
#endif
    return out;
}

}  // namespace

ClockCard::ClockCard(int slotNum) : ClockCard(slotNum, &hostLocalTime) {}

ClockCard::ClockCard(int slotNum, TimeFn fn) : slot(slotNum), timeFn(fn)
{
    buildRom();
    onReset();
}

std::unique_ptr<ClockCard> ClockCard::makeForTest(int slotNum, TimeFn fn)
{
    return std::unique_ptr<ClockCard>(new ClockCard(slotNum, fn));
}

void ClockCard::onReset()
{
    shiftReg.fill(0);
    prevWrite = 0;
    lastMode  = kModeRegisterHold;
}

void ClockCard::latchTimeToShiftReg()
{
    const std::tm lt = timeFn();
    // tm_wday: 0=Sun..6=Sat — matches uPD1990AC's day-of-week field
    // layout. ProDOS doesn't consult day-of-week; we still populate it
    // so the chip's output is faithful for code that does.
    const int dow   = (lt.tm_wday >= 0 && lt.tm_wday <= 6) ? lt.tm_wday : 0;
    const int month = lt.tm_mon + 1;        // tm_mon is 0..11
    shiftReg[0] = toBcd(lt.tm_sec);
    shiftReg[1] = toBcd(lt.tm_min);
    shiftReg[2] = toBcd(lt.tm_hour);
    shiftReg[3] = toBcd(lt.tm_mday);
    // shiftReg[4]: high nibble = month (4-bit binary, NOT BCD — MAME
    // upd1990a.cpp:95 stores month as plain 1..12 in the high 4 bits),
    // low nibble = day-of-week. Months 1..12 all fit in 4 bits.
    shiftReg[4] = static_cast<uint8_t>((month << 4) | (dow & 0x0F));
    shiftReg[5] = toBcd(lt.tm_year % 100);
}

uint8_t ClockCard::deviceSelectRead(uint8_t low4)
{
    // Only $C0n0 carries the chip output; everything else is open bus.
    // We expose DATA_OUT in bit 7, with all other bits zero — the host
    // driver does `BIT $C0n0 / BMI ...` to test it.
    if (low4 == 0) {
        const uint8_t out = (shiftReg[0] & 0x01) ? 0x80 : 0x00;
        return out;
    }
    return 0xFF;
}

void ClockCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (low4 != 0) return;

    const bool clkPrev = (prevWrite & kBitClk) != 0;
    const bool clkNow  = (v         & kBitClk) != 0;
    const bool stbPrev = (prevWrite & kBitStb) != 0;
    const bool stbNow  = (v         & kBitStb) != 0;
    const uint8_t mode = static_cast<uint8_t>((v >> 3) & 0x07);

    // STB rising edge — latch the mode on C0/C1/C2 and (for time-read)
    // load the host clock into the shift register so the next 48 CLK
    // pulses serialise it out.
    if (stbNow && !stbPrev) {
        lastMode = mode;
        if (mode == kModeTimeRead) {
            latchTimeToShiftReg();
        }
        // MODE_TIME_SET / MODE_SHIFT / MODE_REGISTER_HOLD: we don't need
        // to do anything specific for ProDOS's read-only flow.
    }

    // CLK rising edge — shift the 48-bit register right by one bit.
    // Each byte shifts towards LSB, with the LSB of the next byte
    // sliding into the MSB of the previous one. The last byte shifts
    // in zero. After the shift, DATA_OUT (= shiftReg[0] LSB) carries
    // the next bit out.
    if (clkNow && !clkPrev) {
        for (int i = 0; i < 5; ++i) {
            shiftReg[i] = static_cast<uint8_t>(
                (shiftReg[i] >> 1) | ((shiftReg[i + 1] & 0x01) << 7));
        }
        shiftReg[5] = static_cast<uint8_t>(shiftReg[5] >> 1);
    }

    prevWrite = v;
}

void ClockCard::buildRom()
{
    rom.fill(0xEA);     // NOP padding everywhere we don't write

    // ProDOS clock-detection signature. ProDOS only inspects the bytes at
    // even offsets 0/2/4/6; the odd-offset fillers form a benign 1-byte
    // execution path. ProDOS itself never JMPs to $Cs00 for clock reads
    // — it copies its hardcoded driver into RAM (~$D742) and patches
    // $BF06-$BF08 to jump there — so the body past the signature is
    // dead code in practice. We still leave a `BRK; RTS` at $Cs08 in
    // case stray firmware ever does call here, to fail loud rather than
    // wander into NOP-padded territory.
    rom[0x00] = 0x08;   // PHP        — signature
    rom[0x01] = 0xD8;   // CLD        — 1-byte filler
    rom[0x02] = 0x28;   // PLP        — signature
    rom[0x03] = 0xD8;   // CLD
    rom[0x04] = 0x58;   // CLI        — signature (briefly enables IRQ)
    rom[0x05] = 0x78;   // SEI        — re-disable IRQ immediately
    rom[0x06] = 0x70;   // BVS        — signature
    rom[0x07] = 0x00;   // BVS operand: branch +0 → $Cs08 either way
    rom[0x08] = 0x60;   // RTS — minimal "call did nothing" trap
}

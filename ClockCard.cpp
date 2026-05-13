// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ClockCard.h"

#include <ctime>

namespace {

// uPD1990AC mode codes carried on C0/C1/C2 (bits 3,4,5 of the $C0n0
// write). MAME `upd1990a.h:58-73` defines the full 16-entry table; we
// only react to the first four. Tick-pulse modes (TP_64HZ … TP_4096HZ
// and interval timers) require an IRQ output line; their plumbing is
// tracked in TODO §3 and not yet hooked up.
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

int bcdToInt(uint8_t b)
{
    return ((b >> 4) & 0x0F) * 10 + (b & 0x0F);
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
    prevWrite          = 0;
    lastMode           = kModeRegisterHold;
    userOffsetActive   = false;
    userOffsetSeconds  = 0;
}

std::tm ClockCard::effectiveTime() const
{
    std::tm host = timeFn();
    if (!userOffsetActive) return host;
    // Compose `host + offset` via std::mktime / std::localtime so the
    // BCD bytes that come out the shift register reflect the user's
    // set time + elapsed real seconds since the set point. Mirrors
    // MAME's internal time counter which ticks at 1 Hz from
    // `m_timer_clock` once TIME_SET commits.
    std::time_t hostT = std::mktime(&host);
    if (hostT == static_cast<std::time_t>(-1)) return host;
    hostT += userOffsetSeconds;
    std::tm out{};
#if defined(_WIN32)
    localtime_s(&out, &hostT);
#else
    if (std::tm* p = std::localtime(&hostT)) out = *p;
#endif
    return out;
}

void ClockCard::latchTimeToShiftReg()
{
    const std::tm lt = effectiveTime();
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

void ClockCard::commitTimeSetFromShiftReg()
{
    // MAME `upd1990a.cpp:194-225` — MODE_TIME_SET: copy shift_reg into
    // the chip's internal time_counter, then `set_time()`. POM2 has no
    // separate time_counter (we synthesise BCD bytes from `timeFn()`
    // each read), so we instead decode shiftReg → time_t and store the
    // delta vs the current host clock. Future reads compose
    // `timeFn() + userOffsetSeconds` to produce the running set-time.
    std::tm desired{};
    desired.tm_sec  = bcdToInt(shiftReg[0]);
    desired.tm_min  = bcdToInt(shiftReg[1]);
    desired.tm_hour = bcdToInt(shiftReg[2]);
    desired.tm_mday = bcdToInt(shiftReg[3]);
    desired.tm_mon  = ((shiftReg[4] >> 4) & 0x0F) - 1;     // 1..12 → 0..11
    // shiftReg[5] is 2-digit BCD year. Assume 20xx (2000-2099). The
    // uPD1990AC has no century bit; software that needs a different
    // window has to set the offset accordingly. mktime then normalises.
    desired.tm_year = bcdToInt(shiftReg[5]) + 100;         // tm_year = year - 1900
    desired.tm_wday = shiftReg[4] & 0x0F;
    desired.tm_isdst = -1;     // let mktime decide

    const std::time_t desiredT = std::mktime(&desired);
    std::tm hostTm = timeFn();
    const std::time_t hostT = std::mktime(&hostTm);
    if (desiredT == static_cast<std::time_t>(-1)
        || hostT == static_cast<std::time_t>(-1)) {
        // Malformed input — leave the offset alone so a botched
        // TIME_SET doesn't poison subsequent reads.
        return;
    }
    userOffsetSeconds = desiredT - hostT;
    userOffsetActive  = true;
}

uint8_t ClockCard::deviceSelectRead(uint8_t low4)
{
    // DATA_OUT in bit 7, all other bits zero — the host driver does
    // `BIT $C0n0 / BMI ...` to test it. MAME `a2thunderclock.cpp:112-115`
    // ignores the low 4 bits of `offset` entirely and mirrors DATA_OUT
    // across all of $C0n0-$C0nF, so a probe at $C0n5 sees the same
    // bit 7 as $C0n0. POM2 used to return 0xFF for non-zero offsets,
    // which broke some software-detect probes that scan the slot.
    (void)low4;
    return (shiftReg[0] & 0x01) ? 0x80 : 0x00;
}

void ClockCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    if (low4 != 0) return;

    const bool clkPrev = (prevWrite & kBitClk) != 0;
    const bool clkNow  = (v         & kBitClk) != 0;
    const bool stbPrev = (prevWrite & kBitStb) != 0;
    const bool stbNow  = (v         & kBitStb) != 0;
    const uint8_t mode = static_cast<uint8_t>((v >> 3) & 0x07);

    // STB rising edge — latch the mode on C0/C1/C2.
    //   MODE_TIME_READ: load host time (+ optional user offset) into
    //                   shiftReg so the next 48 CLK pulses serialise it.
    //   MODE_TIME_SET : commit the shift register (loaded by the prior
    //                   48 CLK pulses in MODE_SHIFT) as the user-set
    //                   time. MAME `upd1990a.cpp:194-225`.
    //   MODE_SHIFT / MODE_REGISTER_HOLD: no per-edge action — the chip
    //                   just remembers the mode.
    if (stbNow && !stbPrev) {
        lastMode = mode;
        if (mode == kModeTimeRead) {
            latchTimeToShiftReg();
        } else if (mode == kModeTimeSet) {
            commitTimeSetFromShiftReg();
        }
    }

    // CLK rising edge — shift the 48-bit register right by one bit.
    // Each byte shifts towards LSB, with the LSB of the next byte
    // sliding into the MSB of the previous one. The MSB of the last
    // byte gets the DATA_IN bit (bit 0 of $C0n0). After the shift,
    // DATA_OUT (= shiftReg[0] LSB) carries the next bit out.
    //
    // Deliberate divergence from MAME `upd1990a.cpp:312-327`, which
    // gates this shift behind `m_c == MODE_SHIFT`. POM2 keeps the shift
    // lax (any mode) because ProDOS's hardcoded ThunderClock driver
    // pulses CLK while still in MODE_TIME_READ without first switching
    // to MODE_SHIFT — strict gating breaks stock ProDOS reads, and the
    // ThunderClock+ card paired with a real uPD1990AC has been
    // observed to permit this shortcut in practice (the MODE bit just
    // selects DATA_OUT path / time-counter clocking; the shift register
    // is wired to CLK directly). DATA_IN is still latched into the MSB
    // because MODE_TIME_SET expects it.
    if (clkNow && !clkPrev) {
        const uint8_t dataIn = (v & kBitDataIn) ? 1 : 0;
        for (int i = 0; i < 5; ++i) {
            shiftReg[i] = static_cast<uint8_t>(
                (shiftReg[i] >> 1) | ((shiftReg[i + 1] & 0x01) << 7));
        }
        shiftReg[5] = static_cast<uint8_t>(
            (shiftReg[5] >> 1) | (dataIn << 7));
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

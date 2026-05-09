// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ClockCard — ThunderClock+-compatible real-time clock card. Plugs into
// a slot (4 by convention) and emulates the bit-banged uPD1990AC RTC
// chip well enough that ProDOS 8 / 2.4's hardcoded ThunderClock driver
// reads host wall-clock through it. With this in place ProDOS file
// timestamps and BASIC.SYSTEM `DATE` / `TIME` reflect real time.
//
// Detection signature
// -------------------
// ProDOS scans slots 1-7 at boot for the canonical clock-card bytes:
//
//   $Cs00 = $08   (PHP)
//   $Cs02 = $28   (PLP)
//   $Cs04 = $58   (CLI)
//   $Cs06 = $70   (BVS)
//
// On match ProDOS copies its hardcoded ThunderClock driver into RAM
// (around $D742) and rewrites its $BF06-$BF08 vector to JMP into that
// copy. The slot ROM itself is touched only for detection — every
// subsequent CLOCK call talks to the chip via the device-select range
// below, NOT by JMPing back to $Cs00.
//
// uPD1990AC bit-bang protocol
// ---------------------------
// All chip access goes through a single soft-switch register at $C0n0
// (slot 4 → $C0C0):
//
//   write $C0n0:
//     bit 0  DATA_IN     (serial command bit input)
//     bit 1  CLK         (rising edge = shift one bit)
//     bit 2  STB         (rising edge = latch mode bits)
//     bit 3  C0          mode bit 0
//     bit 4  C1          mode bit 1
//     bit 5  C2          mode bit 2
//
//   read  $C0n0:
//     bit 7  DATA_OUT    (LSB of shift register, sampled after CLK)
//
// To read time, the host driver writes mode = 0b011 (MODE_TIME_READ),
// pulses STB to load the current time counter into the 48-bit shift
// register, then pulses CLK 48 times, sampling bit 7 of $C0n0 after
// each rising edge to assemble 6 BCD bytes:
//
//   sec, min, hr, day, (month<<4)|dow, year
//
// The host driver converts BCD → binary and packs into ProDOS DATE/TIME.

#ifndef POM2_CLOCK_CARD_H
#define POM2_CLOCK_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string_view>

class ClockCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    /// Construct with the slot number this card will be plugged into.
    /// The slot bakes into the slot ROM signature offsets but does not
    /// affect the chip protocol.
    explicit ClockCard(int slot = kDefaultSlot);

    int  getSlot() const { return slot; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Clock (ThunderClock+)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override { return rom[low8]; }
    void    onReset() override;

    /// Time-source hook. Defaults to std::time + std::localtime; the
    /// public test factory below swaps in a deterministic source.
    using TimeFn = std::tm (*)();

    /// Test-only factory: build a ClockCard that reads time through
    /// `fn` instead of the host clock. Returns a unique_ptr so the
    /// caller can transfer it to a SlotBus.
    static std::unique_ptr<ClockCard> makeForTest(int slot, TimeFn fn);

private:
    int slot;
    std::array<uint8_t, 256> rom{};
    TimeFn timeFn;

    // ── uPD1990AC bit-bang chip state ─────────────────────────────────
    //
    // The 48-bit shift register lives in 6 bytes, in shift-out order
    // (LSB of byte 0 goes out first):
    //   shiftReg[0] = seconds (BCD)
    //   shiftReg[1] = minutes (BCD)
    //   shiftReg[2] = hours   (BCD)
    //   shiftReg[3] = day     (BCD)
    //   shiftReg[4] = (month << 4) | (day_of_week)
    //   shiftReg[5] = year mod 100 (BCD)
    //
    // STB rising edge with MODE_TIME_READ snapshots host time and loads
    // these six bytes. Each subsequent CLK rising edge shifts the whole
    // bank right by one bit, with DATA_OUT exposing the new LSB of
    // shiftReg[0].
    std::array<uint8_t, 6> shiftReg{};
    uint8_t prevWrite = 0;        // last value seen at $C0n0 write
                                  // (for CLK / STB rising-edge detection)
    uint8_t lastMode  = 0;        // most recently latched C0/C1/C2 (0..7)

    void buildRom();

    /// Snapshot host time into shiftReg as 6 BCD bytes. Called on STB
    /// rising edge when MODE_TIME_READ is selected.
    void latchTimeToShiftReg();

    ClockCard(int slot, TimeFn fn);
};

#endif // POM2_CLOCK_CARD_H

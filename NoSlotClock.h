#pragma once

// NoSlotClock — Dallas DS1216E "SmartWatch" emulation.
//
// The DS1216E is a 28-pin socket that physically sits under a ROM chip
// and intercepts reads to the underlying ROM. It is the canonical
// "clock without using a slot" solution for the Apple //c (which has
// no expansion slots) and for any //e/II+ owner short on slot count.
//
// Protocol (verified against AppleWin's `NoSlotClock.cpp`, which is
// itself derived from Nick Westgate's csa2 post + the Dallas DS1216
// datasheet):
//
//   The host CPU drives the chip via reads to the protected address
//   range ($F800-$FFFF when sat under the Monitor ROM). Address bit
//   A2 selects the operation:
//
//     A2 = 0  → "write" cycle: feed the next 1-bit of the magic key.
//               The bit value comes from A0 (so $F800 sends 0,
//               $F801 sends 1, $F802 sends 0, $F803 sends 1, etc.).
//               64 consecutive matches transition the chip into
//               clock-readout phase.
//     A2 = 1  → "read" cycle: emit the next clock-register bit on D0.
//               During pattern-matching this also RESETS the matcher
//               (host reverting to readout from a half-walked key).
//
//   Mismatch behaviour: a single wrong A0 bit DISABLES further
//   writes — the matcher stays "dead" until the host issues an A2=1
//   read (or until system reset clears state). This is the Dallas
//   datasheet pattern-recognition spec: a bad pattern is sticky.
//
// ProDOS 8 ≥ 2.0.3 + GS/OS scan for a No-Slot Clock by deliberately
// stepping the magic key against the Monitor ROM at $F800-$FFFF and
// observing the readback. POM2 hooks this class into `Memory::memRead`
// for that range when `nsclock_enable` is true (default on; the
// pass-through path is a no-op for software that doesn't trigger the
// magic key).

#include <cstdint>
#include <ctime>

namespace pom2 {

class NoSlotClock
{
public:
    /// Time source. Defaults to `std::time + localtime`; tests swap a
    /// deterministic source via the second constructor.
    using TimeFn = std::tm (*)();

    NoSlotClock();
    explicit NoSlotClock(TimeFn fn);

    void setEnabled(bool on)        { enabled_ = on; }
    bool isEnabled() const          { return enabled_; }

    /// Called by `Memory::memRead` for every read in the watched range
    /// (`$F800-$FFFF` by default). `romByte` is what the ROM/RAM
    /// underneath would normally return. The intercept returns the
    /// byte the CPU actually sees: pass-through except during clock-
    /// readout phase, where D0 carries the next clock bit.
    uint8_t interceptRead(uint16_t addr, uint8_t romByte);

    /// State accessor for UI / tests.
    enum class Phase { Idle, MatchingKey, ReadingClock };
    Phase   phase() const;
    int     keyBitsMatched() const  { return bitsMatched_; }
    int     clockBitsRead()  const  { return bitsRead_;    }

    /// Force a hardware-style reset: re-enables pattern matching and
    /// clears the matcher state. Not auto-called on CPU softReset
    /// because the DS1216E is battery-backed on real hardware and
    /// keeps state across resets.
    void    onReset();

    /// AppleWin-parity magic key — `CNoSlotClock::kClockInitSequence`
    /// in `AppleWin/source/NoSlotClock.h`. The 64 bits are LSB-first
    /// transmission of the Dallas DS1216E datasheet's 8-byte canonical
    /// sequence  C5 3A A3 5C C5 3A A3 5C  packed little-endian.
    static constexpr uint64_t kMagicKey = 0x5CA33AC55CA33AC5ULL;

private:
    void    loadClockSnapshot();
    static std::tm defaultTimeFn();

    TimeFn   timeFn_       = nullptr;
    bool     enabled_      = true;
    bool     writeEnabled_ = true;     // false after a mismatched key bit
                                       //   (sticky; cleared by A2=1 read)
    bool     readingClock_ = false;    // 64-bit clock-out phase active
    uint8_t  bitsMatched_  = 0;        // 0..63 (key-feed progress)
    uint8_t  bitsRead_     = 0;        // 0..63 (clock-out progress)
    uint64_t clockShift_   = 0;        // shifted out LSB-first to D0
};

}  // namespace pom2

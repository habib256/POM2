// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Ay3_8910 — GI/Microchip AY-3-8910 / AY-3-8913 PSG register bank +
// VIA-side control bus decoder. The audio synthesis state (counters,
// LFSR, envelope step) lives on the audio thread inside each card's
// AudioSrc — NOT here. This struct is touched by both threads; the
// owning card serialises access with a mutex.
//
// Extracted 2026-05-27 from MockingboardCard's private nested struct so
// PhasorCard can drive four instances over the same bus contract.
//
// AY-3-8910 vs AY-3-8913: identical synthesis core; the 8913 omits the
// two 8-bit I/O ports (registers R14/R15). Mockingboard wires R14/R15
// unused so the same struct serves both. Phasor uses 8913s — we keep
// R14/R15 in the bank for code symmetry, they just stay zero in
// practice.
//
// PB → AY control bus map (Mockingboard A/C + Phasor):
//   PB0 → BC1
//   PB1 → BDIR
//   PB2 → /RESET (active LOW)
// Command encoding (BDIR, BC1):
//   00  INACTIVE   — no bus action
//   01  READ       — rare; music drivers don't read
//   10  WRITE      — write PA byte to latched register
//   11  LATCH ADDR — latch PA[3:0] as the next register address

#ifndef POM2_AY3_8910_H
#define POM2_AY3_8910_H

#include <cstdint>
#include <cstring>

namespace pom2 {

struct Ay3_8910
{
    static constexpr int kAyNumRegs = 16;

    // PB bit positions for AY control bus.
    static constexpr uint8_t kPbBitBc1   = 0x01;
    static constexpr uint8_t kPbBitBdir  = 0x02;
    static constexpr uint8_t kPbBitReset = 0x04;

    uint8_t regs[kAyNumRegs] = {0};
    uint8_t latchedAddr = 0;

    // PB control state captured on the last VIA strobe — for transition
    // detection in applyControl.
    uint8_t prevCommand = 0;     // {BDIR, BC1} as a 2-bit command

    // Per-command counters (diagnostic — surfaced via card peek APIs).
    // Tick only on real {BDIR,BC1} edges so a held strobe reports once.
    uint32_t latchCount       = 0;
    uint32_t writeStrobeCount = 0;
    uint32_t readStrobeCount  = 0;
    uint32_t inactiveCount    = 0;

    void reset()
    {
        // MAME `ay8910.cpp ay8910_reset_ym` clears regs 0..AY_PORTA-1
        // (= 0..13) and leaves R14/R15 untouched. Mockingboard / Phasor
        // wire R14/R15 unused so this is academic, but `getAyRegister`
        // peeks would diverge from MAME if we wiped all 16.
        std::memset(regs, 0, 14);
        latchedAddr = 0;
        prevCommand = 0;
    }

    enum ApplyResult { NoChange, ResetOnly, Wrote };

    /// React to a VIA Port B (and, on Latch/Write commands, also Port A)
    /// change. `pa` is the AY data bus (VIA Port A output bits driven by
    /// DDRA), `pb` is the VIA Port B output after DDRB masking.
    /// !RESET (PB2) is active-low: while held low the AY stays in reset
    /// and every register reads zero. Reported as `ResetOnly` so the
    /// diagnostic panel can separate "music driver clearing the chip"
    /// from "music driver delivered a register-store strobe".
    ApplyResult applyControl(uint8_t pa, uint8_t pb)
    {
        if ((pb & kPbBitReset) == 0) {
            reset();
            return ApplyResult::ResetOnly;
        }
        const uint8_t cmd = static_cast<uint8_t>(
            ((pb & kPbBitBdir) ? 0x02 : 0) |
            ((pb & kPbBitBc1)  ? 0x01 : 0));
        // MAME `mockingboard.cpp:391-410 via_psg_ctrl` fires on every PB
        // write — no edge debounce. A music driver holding BDIR through
        // multiple PA changes legitimately re-strobes the same AY
        // register with each new data byte. Edge tracking is for
        // diagnostic counters only.
        ApplyResult result = ApplyResult::NoChange;
        const bool edge = (cmd != prevCommand);
        switch (cmd) {
        case 0b11:    // LATCH ADDR
            if (edge) ++latchCount;
            latchedAddr = static_cast<uint8_t>(pa & 0x0F);
            break;
        case 0b10:    // WRITE
            if (edge) ++writeStrobeCount;
            regs[latchedAddr & 0x0F] = pa;
            result = ApplyResult::Wrote;
            break;
        case 0b01:    // READ
            if (edge) ++readStrobeCount;
            break;
        case 0b00:
        default:
            if (edge) ++inactiveCount;
            break;
        }
        prevCommand = cmd;
        return result;
    }
};

} // namespace pom2

#endif // POM2_AY3_8910_H

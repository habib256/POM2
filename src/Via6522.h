// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Via6522 — minimal 65C22 VIA model, used by every POM2 card that fronts
// AY-3-8910/8913 PSGs (Mockingboard, Phasor, Echo+). Originally lived as
// a private nested struct inside MockingboardCard; extracted 2026-05-27
// so PhasorCard can share the same VIA model verbatim without
// duplicating the timer / IFR / Port-A/B logic.
//
// Scope: T1 (both modes), T2 (one-shot phase-2), IFR/IER, Port A/B
// output latches + DDR. NOT modelled (matching the original MB scope):
// SR shift register, CA1/CA2/CB1/CB2 handshake, PB6 pulse counting for
// T2 (acknowledged but never ticks — no POM2 card wires PB6 externally).
//
// Header-only: every method is `inline`, no `Via6522.cpp` to link.
// Cards include this header, instantiate via `std::make_unique<Via6522>`,
// and reach the public methods directly.
//
// Verbatim port of MAME `machine/6522via.cpp` for the subset above. The
// detailed per-method MAME line references live next to each switch
// case below.

#ifndef POM2_VIA6522_H
#define POM2_VIA6522_H

#include "ByteIO.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace pom2 {

struct Via6522
{
    // ─── Register layout (WDC W65C22 datasheet) ──────────────────────────
    enum : uint8_t {
        VIA_ORB    = 0x0,  // Output Reg B / Input Reg B
        VIA_ORA    = 0x1,  // Output Reg A (with handshake)
        VIA_DDRB   = 0x2,
        VIA_DDRA   = 0x3,
        VIA_T1CL   = 0x4,  // Timer 1 counter low
        VIA_T1CH   = 0x5,  // Timer 1 counter high
        VIA_T1LL   = 0x6,  // Timer 1 latch low
        VIA_T1LH   = 0x7,  // Timer 1 latch high
        VIA_T2CL   = 0x8,
        VIA_T2CH   = 0x9,
        VIA_SR     = 0xA,
        VIA_ACR    = 0xB,
        VIA_PCR    = 0xC,
        VIA_IFR    = 0xD,
        VIA_IER    = 0xE,
        VIA_ORANH  = 0xF,  // Output Reg A no handshake
    };

    // IFR / IER bit positions.
    static constexpr uint8_t IFR_CA1 = 0x02;
    static constexpr uint8_t IFR_T2  = 0x20;
    static constexpr uint8_t IFR_T1  = 0x40;
    static constexpr uint8_t IFR_ANY = 0x80;   // computed on read

    // ─── State ───────────────────────────────────────────────────────────
    uint8_t portAOut = 0x00;
    uint8_t portBOut = 0x00;
    uint8_t ddrA = 0x00;
    uint8_t ddrB = 0x00;
    uint8_t acr  = 0x00;
    uint8_t pcr  = 0x00;
    uint8_t sr   = 0x00;
    // Timer 1 latches and counter. `t1Counter` is signed-extended to 32
    // bits so we can detect the underflow transition (16-bit counter goes
    // 0 → -1, i.e. the pulse fires when we count *through* zero).
    uint16_t t1Latch = 0xFFFF;
    int32_t  t1Counter = 0xFFFF;
    bool     t1FireArmed = false;  // post-reset T1 doesn't fire until SW loads T1CH
    // Timer 2. One-shot on phase-2 (ACR.bit5 PB6 pulse-counting NOT
    // modelled — no POM2 card drives PB6 externally). Unlocks Ultima IV
    // Echo+ speech driver, FrenchTouch sample demos.
    uint8_t  t2ll       = 0xFF;
    uint16_t t2Latch    = 0xFFFF;  // {t2lh:t2ll}, set by T2CH write
    int32_t  t2Counter  = 0xFFFF;
    bool     t2Active   = false;   // armed → fires one IRQ on underflow
    // IFR / IER store only per-source bits (0..6); bit 7 computed on read.
    uint8_t ifr = 0x00;
    uint8_t ier = 0x00;

    inline void reset()
    {
        portAOut = portBOut = 0;
        ddrA = ddrB = 0;
        acr  = pcr = sr = 0;
        t1Latch = 0xFFFF;
        t1Counter = 0xFFFF;
        t1FireArmed = false;
        t2ll       = 0xFF;
        t2Latch    = 0xFFFF;
        t2Counter  = 0xFFFF;
        t2Active   = false;
        ifr = 0;
        ier = 0;
    }

    // ─── Snapshot (rewind) ───────────────────────────────────────────────
    // Fixed 24-byte layout of the full register/timer state. Lazily-synced
    // counters are captured as-is (the next syncToCpuCycle re-advances them,
    // exactly as it would have on the live machine).
    static constexpr std::size_t kSnapshotBytes = 24;
    inline void appendSnapshot(std::vector<uint8_t>& o) const
    {
        byteio::putU8(o, portAOut); byteio::putU8(o, portBOut);
        byteio::putU8(o, ddrA);     byteio::putU8(o, ddrB);
        byteio::putU8(o, acr);      byteio::putU8(o, pcr); byteio::putU8(o, sr);
        byteio::putU16(o, t1Latch); byteio::putU32(o, static_cast<uint32_t>(t1Counter));
        byteio::putU8(o, t1FireArmed ? 1 : 0);
        byteio::putU8(o, t2ll);     byteio::putU16(o, t2Latch);
        byteio::putU32(o, static_cast<uint32_t>(t2Counter));
        byteio::putU8(o, t2Active ? 1 : 0);
        byteio::putU8(o, ifr);      byteio::putU8(o, ier);
    }
    inline void loadSnapshot(const uint8_t* d)   // caller ensures >= kSnapshotBytes
    {
        byteio::Reader r(d, kSnapshotBytes);
        portAOut = r.u8(); portBOut = r.u8(); ddrA = r.u8(); ddrB = r.u8();
        acr = r.u8(); pcr = r.u8(); sr = r.u8();
        t1Latch = r.u16(); t1Counter = static_cast<int32_t>(r.u32());
        t1FireArmed = r.u8() != 0;
        t2ll = r.u8(); t2Latch = r.u16(); t2Counter = static_cast<int32_t>(r.u32());
        t2Active = r.u8() != 0;
        ifr = r.u8(); ier = r.u8();
    }

    // Composed Port reads: input pins (DDR=0) pulled high (Mockingboard /
    // Phasor have no inputs wired), output pins reflect the latch.
    inline uint8_t readPortB() const
    {
        const uint8_t input = 0xFF;
        return (portBOut & ddrB) | (input & ~ddrB);
    }
    inline uint8_t readPortA() const
    {
        const uint8_t input = 0xFF;
        return (portAOut & ddrA) | (input & ~ddrA);
    }

    inline bool irqOut() const { return (ifr & ier & 0x7F) != 0; }

    inline uint8_t computedIfr() const
    {
        return static_cast<uint8_t>(
            (ifr & 0x7F) | (irqOut() ? IFR_ANY : 0));
    }

    // T1 mode bits live in ACR bits 7..6.
    inline bool t1Continuous() const { return (acr & 0x40) != 0; }

    // 6522 read — `reg` in 0..15. Some reads have side effects
    // (clearing IFR.T1 on T1CL/T1CH read).
    inline uint8_t read(uint8_t reg)
    {
        switch (reg & 0x0F) {
        case VIA_ORB:    return readPortB();
        case VIA_ORA:    // reading ORA also clears handshake on real HW;
                         // we don't model handshake.
                         return readPortA();
        case VIA_DDRB:   return ddrB;
        case VIA_DDRA:   return ddrA;
        case VIA_T1CL: {
            ifr &= ~IFR_T1;
            return static_cast<uint8_t>(t1Counter & 0xFF);
        }
        case VIA_T1CH:   return static_cast<uint8_t>((t1Counter >> 8) & 0xFF);
        case VIA_T1LL:   return static_cast<uint8_t>(t1Latch & 0xFF);
        case VIA_T1LH:   return static_cast<uint8_t>((t1Latch >> 8) & 0xFF);
        case VIA_T2CL: {
            // T2CL read clears IFR.T2 (MAME `6522via.cpp:590-594`).
            ifr &= ~IFR_T2;
            return static_cast<uint8_t>(t2Counter & 0xFF);
        }
        case VIA_T2CH:   return static_cast<uint8_t>((t2Counter >> 8) & 0xFF);
        case VIA_SR:     return sr;
        case VIA_ACR:    return acr;
        case VIA_PCR:    return pcr;
        case VIA_IFR:    return computedIfr();
        case VIA_IER:    return static_cast<uint8_t>(ier | 0x80);
        case VIA_ORANH:  return readPortA();
        default:         return 0xFF;
        }
    }

    // 6522 write. Returns a bit-pattern of which "events" happened so
    // the caller can react: bit 0 = Port B output changed, bit 1 = Port
    // A output changed.
    inline uint8_t write(uint8_t reg, uint8_t v)
    {
        uint8_t events = 0;
        switch (reg & 0x0F) {
        case VIA_ORB: {
            const uint8_t prev = portBOut;
            portBOut = v;
            if ((prev & ddrB) != (v & ddrB)) events |= 0x01;
            break;
        }
        case VIA_ORA: {
            const uint8_t prev = portAOut;
            portAOut = v;
            if ((prev & ddrA) != (v & ddrA)) events |= 0x02;
            break;
        }
        case VIA_DDRB: {
            const uint8_t prev = portBOut & ddrB;
            ddrB = v;
            if ((portBOut & ddrB) != prev) events |= 0x01;
            break;
        }
        case VIA_DDRA: {
            const uint8_t prev = portAOut & ddrA;
            ddrA = v;
            if ((portAOut & ddrA) != prev) events |= 0x02;
            break;
        }
        case VIA_T1CL:
        case VIA_T1LL:
            t1Latch = (t1Latch & 0xFF00) | v;
            break;
        case VIA_T1CH:
            // Latch high, transfer latches into counter, start timer,
            // clear IFR.T1.
            t1Latch = (t1Latch & 0x00FF) | (static_cast<uint16_t>(v) << 8);
            t1Counter = t1Latch;
            t1FireArmed = true;
            ifr &= ~IFR_T1;
            break;
        case VIA_T1LH:
            // Latch high only: NO counter transfer, NO IFR side effect.
            // T1 IFR is cleared only by T1CL read or T1CH write — NOT by
            // T1LH write (MAME 6522via.cpp T1L-H case).
            t1Latch = (t1Latch & 0x00FF) | (static_cast<uint16_t>(v) << 8);
            break;
        case VIA_T2CL:
            // Store the low latch only — no effect on the running counter
            // (MAME `6522via.cpp:764-766`).
            t2ll    = v;
            t2Latch = static_cast<uint16_t>((t2Latch & 0xFF00) | v);
            break;
        case VIA_T2CH:
            // Latch high, transfer {t2lh:t2ll} into counter, clear IFR.T2,
            // arm T2 (MAME `6522via.cpp:767-782`).
            t2Latch    = static_cast<uint16_t>((static_cast<uint16_t>(v) << 8) | t2ll);
            t2Counter  = t2Latch;
            ifr       &= ~IFR_T2;
            t2Active   = true;
            break;
        case VIA_SR:    sr  = v; break;
        case VIA_ACR:   acr = v; break;
        case VIA_PCR:   pcr = v; break;
        case VIA_IFR:
            // Writing 1s to IFR clears those bits (only 0..6 user-clearable).
            ifr &= ~(v & 0x7F);
            break;
        case VIA_IER:
            // Bit 7 selects set vs clear; bits 0..6 are the mask.
            if (v & 0x80) ier |= (v & 0x7F);
            else          ier &= ~(v & 0x7F);
            break;
        case VIA_ORANH: {
            const uint8_t prev = portAOut;
            portAOut = v;
            if ((prev & ddrA) != (v & ddrA)) events |= 0x02;
            break;
        }
        default: break;
        }
        return events;
    }

    /// Strobe an external CA1 input edge — used by Mockingboard "Sound II"
    /// when the on-board SSI263's A/!R signal toggles. PCR bit 0 selects
    /// the active edge: 0 = negative-going, 1 = positive-going. We model
    /// the canonical Mockingboard wiring (SSI263 A/!R inverted into CA1
    /// → 0→1 of A/!R = negative edge on CA1), so `setCa1NegativeEdge()`
    /// sets IFR.CA1 only when PCR.0 == 0. If the host CPU has IER.CA1
    /// (= IFR_CA1 bit) enabled, `irqOut()` will then go high.
    /// AppleWin parity: `if ((GetPCR(m_device) & 1) == 0) UpdateIFR(0, IxR_SSI263)`.
    inline void setCa1NegativeEdge()
    {
        if ((pcr & 0x01) == 0) ifr |= IFR_CA1;
    }

    /// Symmetric helper for cards that need the opposite polarity.
    inline void setCa1PositiveEdge()
    {
        if ((pcr & 0x01) != 0) ifr |= IFR_CA1;
    }

    // Advance T1 (and T2 in one-shot phase-2 mode) by `cycles` 1.0227 MHz
    // ticks. Sets IFR.T1/T2 on underflow and (T1 continuous mode) reloads
    // from the latch automatically.
    inline bool advance(int cycles)
    {
        if (cycles <= 0) return false;
        bool fired = false;
        t1Counter -= cycles;
        while (t1Counter < 0) {
            if (t1FireArmed) {
                ifr |= IFR_T1;
                fired = true;
                if (!t1Continuous()) {
                    t1FireArmed = false;
                }
            }
            if (t1Continuous()) {
                // +3 matches MAME `6522via.cpp:534,104` reload constant
                // `TIMER1_VALUE + IFR_DELAY` (IFR_DELAY = 3 = latch-to-
                // counter copy + PB7 pulse pair). Collapse the reload
                // arithmetically: a degenerate tiny latch (e.g. 0 → period 3)
                // under a large `cycles` (clamped to ~2.1e9 on a sync desync)
                // would otherwise spin this loop hundreds of millions of
                // times. IFR_T1 has already latched (idempotent above), so
                // jump t1Counter forward in one int64 step to ≥ 0.
                const int64_t period  = static_cast<int64_t>(t1Latch) + 3;
                const int64_t deficit = -static_cast<int64_t>(t1Counter);   // > 0
                const int64_t periods = deficit / period + 1;
                t1Counter = static_cast<int32_t>(
                    static_cast<int64_t>(t1Counter) + periods * period);
            } else {
                t1Counter += 0x10000;
            }
        }
        // T2 advance — phase-2 mode only.
        if ((acr & 0x20) == 0) {
            t2Counter -= cycles;
            while (t2Counter < 0) {
                if (t2Active) {
                    ifr |= IFR_T2;
                    fired = true;
                    t2Active = false;
                }
                t2Counter += 0x10000;
            }
        }
        return fired;
    }
};

} // namespace pom2

#endif // POM2_VIA6522_H

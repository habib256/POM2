// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Mockingboard.h"

#include "CpuClock.h"
#include "M6502.h"

#include <algorithm>
#include <cstring>

namespace {

// ─── AY-3-8910 amplitude table ───────────────────────────────────────────
//
// Logarithmic 4-bit volume → linear amplitude. Sourced from MAME
// `src/devices/sound/ay8910.cpp`'s `build_single_table(normalize=1)` with
// `ay8910_param` (Westcott 2001 measurements). Canonical Apple II / CPC /
// Spectrum reference. Index 0 is silence; index 15 is the per-channel
// peak. Three channels at peak would clip a single AY at roughly 3.0 —
// the audio mixer's clamp catches that.
constexpr float kAyVolumeTable[16] = {
    0.0000f, 0.0105f, 0.0154f, 0.0223f, 0.0321f, 0.0468f, 0.0635f, 0.1061f,
    0.1319f, 0.2164f, 0.2974f, 0.3909f, 0.5128f, 0.6371f, 0.8186f, 1.0000f
};

// AY-3-8910 input clock on the Mockingboard — pin 22 (CLOCK) is wired to
// the slot's phase 0 clock, i.e. the Apple II 1.0227 MHz CPU clock.
//
// MAME `ay8910.cpp:1077` runs its synthesis stream at `clock/8` and
// increments tone/noise counters by 1 per sample. Datasheet output
// freq is `clock/(16*TP)` — produced by a divide-by-2 T-flop on the
// counter output (one toggle per `TP` counter ticks → output cycle =
// `2*TP/(clock/8) = TP/(clock/16)` sec). POM2 matches MAME by stepping
// counters at clock/8 and toggling on `counter >= TP`; the implicit
// /2 lives in the toggle. Earlier POM2 versions used clock/16 which
// produced **one octave too low** on every Mockingboard note. Fixed
// 2026-05-14.
constexpr float kAyClockHz       = static_cast<float>(POM2_CPU_CLOCK_HZ);
constexpr float kAyToneStepHz    = kAyClockHz / 8.0f;    // ~127.8 kHz
constexpr float kAyNoiseStepHz   = kAyClockHz / 8.0f;    // ~127.8 kHz

// Mockingboard PB → AY control pin map. Reading PB0..2 gives the AY
// command nibble; bit 0 doubles as !RESET (active low).
// 6522 Port B → AY-3-8910/8913 control bus, matching the schematic of
// the real Sweet Microsystems Mockingboard A/C and Phasor cards. Verified
// against AppleWin `Mockingboard.cpp:193` and the AY-3-8913 datasheet:
//   PB0 → BC1
//   PB1 → BDIR
//   PB2 → /RESET (active LOW; 1 = chip running, 0 = chip held in reset)
//   PB3..7 unused on Mockingboard A/C (chip-select bits on Phasor)
//
// Earlier versions of this file had PB0=/RESET and PB2=BC1 which inverted
// the AY's view of the bus: every "INACTIVE" pulse a music driver
// emitted between LATCH and WRITE (typically PB=$04 on real HW)
// was misinterpreted as /RESET-asserted by POM2 and wiped the AY
// register bank. Nox Archaist, Ultima IV and every other IRQ-driven
// music driver therefore sounded silent — the AY regs got cleared
// before the next WRITE strobe could land. Fixed 2026-05-14.
constexpr uint8_t kPbBitBc1   = 0x01;   // PB0 → AY BC1
constexpr uint8_t kPbBitBdir  = 0x02;   // PB1 → AY BDIR
constexpr uint8_t kPbBitReset = 0x04;   // PB2 → AY /RESET (active low)

// AY-3-8910 register count.
constexpr int kAyNumRegs = 16;

// 6522 VIA register indices.
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

// IFR / IER bit positions (see WDC W65C22 datasheet).
constexpr uint8_t IFR_T2   = 0x20;
constexpr uint8_t IFR_T1   = 0x40;
constexpr uint8_t IFR_ANY  = 0x80;   // computed on read

}  // namespace

// ─── Forward types ───────────────────────────────────────────────────────
//
// Definitions for the VIA, AY, and AudioSrc subdevices. Kept in this TU
// (not the header) so the rest of the codebase doesn't compile their
// internals on every include.

struct MockingboardCard::Via6522
{
    // Register file. `regs[]` is the canonical view at `read()` time
    // for everything except T1 (whose effective read goes through the
    // counter), Port A/B (which combine DDR + output latch + input
    // pins), and IFR/IER (which compute bit 7 dynamically).
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
    bool     t1FireArmed = true;   // one-shot: fires only after a fresh load
    // Timer 2. Verbatim port of MAME `6522via.cpp:761-782` (write)
    // and `:588-625` (read). T2 is a one-shot timer on phase-2 (the
    // ACR.bit5 PB6 pulse-counting mode is *not* modelled — POM2
    // exposes no PB6 pin so that mode would never tick). Unlocks
    // Ultima IV's Echo+ speech driver and FrenchTouch sample demos.
    uint8_t  t2ll   = 0xFF;
    uint16_t t2Latch  = 0xFFFF;     // {t2lh:t2ll}, set by T2CH write
    int32_t  t2Counter = 0xFFFF;
    bool     t2Active  = false;     // armed → fires one IRQ on underflow
    // IFR / IER — we store only the per-source bits (bits 0..6); bit 7
    // is computed at read time.
    uint8_t ifr = 0x00;
    uint8_t ier = 0x00;

    void reset()
    {
        portAOut = portBOut = 0;
        ddrA = ddrB = 0;
        acr  = pcr = sr = 0;
        t1Latch = 0xFFFF;
        t1Counter = 0xFFFF;
        t1FireArmed = false;     // post-reset T1 doesn't fire until loaded
        t2ll       = 0xFF;
        t2Latch    = 0xFFFF;
        t2Counter  = 0xFFFF;
        t2Active   = false;
        ifr = 0;
        ier = 0;
    }

    // Composed Port B / Port A reads: input pins (DDR=0) are pulled
    // high (Mockingboard has no inputs wired), output pins reflect the
    // latch.
    uint8_t readPortB() const
    {
        const uint8_t input = 0xFF;     // no inputs wired → all-ones
        return (portBOut & ddrB) | (input & ~ddrB);
    }
    uint8_t readPortA() const
    {
        const uint8_t input = 0xFF;
        return (portAOut & ddrA) | (input & ~ddrA);
    }

    bool irqOut() const
    {
        return (ifr & ier & 0x7F) != 0;
    }

    uint8_t computedIfr() const
    {
        return static_cast<uint8_t>(
            (ifr & 0x7F) | (irqOut() ? IFR_ANY : 0));
    }

    // T1 mode bits live in ACR bits 7..6.
    bool t1Continuous() const { return (acr & 0x40) != 0; }

    // 6522 read — `reg` in 0..15. Some reads have side effects
    // (clearing IFR.T1 on T1CL/T1CH).
    uint8_t read(uint8_t reg)
    {
        switch (reg & 0x0F) {
        case VIA_ORB:    return readPortB();
        case VIA_ORA:    /* reading ORA also clears handshake state on real
                            HW; we don't model handshake.            */
                          return readPortA();
        case VIA_DDRB:   return ddrB;
        case VIA_DDRA:   return ddrA;
        case VIA_T1CL: {
            // Reading T1CL clears the T1 interrupt flag.
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
    uint8_t write(uint8_t reg, uint8_t v)
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
            // Write T1CH: latch high byte, transfer latches into counter,
            // start timer, clear IFR.T1.
            t1Latch = (t1Latch & 0x00FF) | (static_cast<uint16_t>(v) << 8);
            t1Counter = t1Latch;
            t1FireArmed = true;
            ifr &= ~IFR_T1;
            break;
        case VIA_T1LH:
            // Latch high (no transfer to counter, no IFR clear) — except
            // a real 6522 *does* clear IFR.T1 on T1LH write, per WDC
            // datasheet table 4. Mocked the same way for parity.
            t1Latch = (t1Latch & 0x00FF) | (static_cast<uint16_t>(v) << 8);
            ifr &= ~IFR_T1;
            break;
        case VIA_T2CL:
            // T2CL write: store the low latch. No effect on the running
            // counter — only T2CH write reloads (MAME
            // `6522via.cpp:764-766`).
            t2ll    = v;
            t2Latch = static_cast<uint16_t>((t2Latch & 0xFF00) | v);
            break;
        case VIA_T2CH:
            // T2CH write: latch high, transfer {t2lh:t2ll} into the
            // counter, clear IFR.T2, arm T2 (MAME `6522via.cpp:767-782`).
            // PB6 pulse-counting mode (ACR bit 5 == 1) is acknowledged
            // but never ticks in POM2 because no card we model drives
            // the PB6 pin from outside.
            t2Latch    = static_cast<uint16_t>((static_cast<uint16_t>(v) << 8) | t2ll);
            t2Counter  = t2Latch;
            ifr       &= ~IFR_T2;
            t2Active   = true;
            break;
        case VIA_SR:    sr  = v; break;
        case VIA_ACR:   acr = v; break;
        case VIA_PCR:   pcr = v; break;
        case VIA_IFR:
            // Writing 1s to IFR clears those bits (only bits 0..6 are
            // user-clearable; bit 7 is read-only on this register).
            ifr &= ~(v & 0x7F);
            break;
        case VIA_IER:
            // Bit 7 of the value selects set vs clear; bits 0..6 are the
            // mask. Writing $C0 sets T1; writing $40 clears T1.
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

    // Advance T1 by `cycles` 1.0227 MHz ticks. Sets IFR.T1 on underflow
    // and (in continuous mode) reloads from the latch automatically.
    // Returns true if T1 fired this slice (caller doesn't usually care —
    // updateIrq() will see ifr).
    bool advance(int cycles)
    {
        if (cycles <= 0) return false;
        bool fired = false;
        // The 6522 counts down on every phase-2 falling edge; underflow
        // fires when counter == -1 (i.e. 0 → 0xFFFF transition + 1 extra
        // cycle). For our purposes the +1 is absorbed into `<= 0`.
        t1Counter -= cycles;
        // Loop: in continuous mode we may underflow many times in one
        // slice (e.g. budget = 16384 cycles, period = 100 → 163 fires).
        while (t1Counter < 0) {
            if (t1FireArmed) {
                ifr |= IFR_T1;
                fired = true;
                if (!t1Continuous()) {
                    // One-shot: don't fire again until SW reloads T1CH.
                    // Counter keeps free-running below.
                    t1FireArmed = false;
                }
            }
            // Continuous mode: reload from latch. One-shot mode: keep
            // counting down through 0xFFFF to give SW visibility into
            // the post-fire counter (matches real 6522 behaviour).
            if (t1Continuous()) {
                t1Counter += static_cast<int32_t>(t1Latch) + 3;
                // +3 matches MAME `6522via.cpp:534,104` reload constant
                // `TIMER1_VALUE + IFR_DELAY` where `IFR_DELAY = 3`. The
                // 3 cycles account for the latch-to-counter copy plus
                // the PB7 pulse pair (the +2 previously used here was
                // off by one — within 0.5 % on typical music T1 periods
                // but deviates from MAME timing on cycle-counting tests).
            } else {
                t1Counter += 0x10000;       // wrap 16-bit
            }
        }

        // T2 advance — one-shot phase-2 mode only (ACR.bit5 == 0). PB6
        // pulse-counting (ACR.bit5 == 1) is acknowledged but never
        // ticks: no Mockingboard driver wires PB6 externally, so the
        // counter would simply hold while POM2 still services the
        // armed/disarmed semantics correctly via T2CH writes.
        if ((acr & 0x20) == 0) {
            t2Counter -= cycles;
            // Underflow fires AT MOST ONCE per arming — real chip: "an
            // underflow causes only one interrupt between T2CH writes"
            // (MAME `6522via.cpp:107-112`). After firing, the counter
            // keeps free-running through 0xFFFF for SW visibility.
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

// ─── AY-3-8910 ────────────────────────────────────────────────────────────
//
// Register-bank holder. The synthesis state (counters, LFSR, envelope
// step) lives on the audio thread inside AudioSrc — not here. This
// class is touched by both threads, so all access goes through the
// parent card's `mtx`.
struct MockingboardCard::Ay3_8910
{
    uint8_t regs[kAyNumRegs] = {0};
    uint8_t latchedAddr = 0;

    // PB control state, captured the last time the VIA toggled the
    // command bus. Stored so `applyControl` can detect transitions.
    uint8_t prevCommand = 0;     // {BC1, BDIR} as a 2-bit command

    void reset()
    {
        // MAME `ay8910.cpp ay8910_reset_ym` clears regs 0..AY_PORTA-1
        // (= 0..13) and leaves R14/R15 (I/O ports A/B) untouched. The
        // Mockingboard wires R14/R15 unused so this is academic on
        // music drivers, but a peek via getAyRegister(chip, 14|15)
        // would diverge from MAME if we wiped all 16.
        std::memset(regs, 0, 14);
        latchedAddr = 0;
        prevCommand = 0;
    }

    /// React to a VIA Port B change (or, on Latch/Write, also a Port A
    /// change). `pa` is the AY data bus (VIA Port A output bits driven
    /// by DDRA), `pb` is the VIA Port B output (after DDRB masking).
    /// Returns true if a register was written (so the audio thread
    /// might want to recompute envelope shape, etc.).
    // !RESET (PB0) is active-low: while held low the AY stays in reset
    // and every register reads zero. We mirror that behaviour here, but
    // report it as a distinct `ResetOnly` event so the diagnostic panel
    // can separate "the music driver is clearing the chip" from "the
    // music driver successfully delivered a register-store strobe".
    enum ApplyResult { NoChange, ResetOnly, Wrote };
    ApplyResult applyControl(uint8_t pa, uint8_t pb)
    {
        if ((pb & kPbBitReset) == 0) {
            reset();
            return ApplyResult::ResetOnly;
        }
        const uint8_t cmd = static_cast<uint8_t>(
            ((pb & kPbBitBdir) ? 0x02 : 0) |
            ((pb & kPbBitBc1)  ? 0x01 : 0));
        // MAME `mockingboard.cpp:391-410 via_psg_ctrl` fires on every
        // PB write — no edge debounce. A music driver that holds BDIR
        // through multiple PA changes legitimately re-strobes the same
        // AY register with each new data byte; the previous edge-only
        // path silently dropped those re-strobes. Diagnostic counters
        // (latchCount/writeStrobeCount/etc.) only tick on real
        // {BDIR,BC1} edges so a held strobe still reports as one.
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
        case 0b01:    // READ — Mockingboard drivers don't read.
            if (edge) ++readStrobeCount;
            break;
        case 0b00:
        default:
            if (edge) ++inactiveCount;
            break;    // INACTIVE
        }
        prevCommand = cmd;
        return result;
    }

    // Per-command counters surfaced via MockingboardCard::getAyCommandCount.
    // Tells us exactly which strobes the music driver is emitting — if
    // `writeStrobeCount` stays 0 while `latchCount` is non-zero we know
    // the driver issues addresses but never the data write that follows.
    uint32_t latchCount       = 0;
    uint32_t writeStrobeCount = 0;
    uint32_t readStrobeCount  = 0;
    uint32_t inactiveCount    = 0;
};

// ─── AudioSrc ─────────────────────────────────────────────────────────────
//
// Inner AudioSource, owned by the card. Holds the synthesis state for
// both AY chips and runs entirely on the audio thread (AudioDevice's
// callback). Per call, locks the parent mutex briefly to snapshot both
// AYs' register banks, then synthesises mono samples summed from both
// chips.
struct MockingboardCard::AudioSrc : public AudioSource, public RateAware
{
    explicit AudioSrc(MockingboardCard* p) : parent(p) {}

    MockingboardCard* parent;

    std::atomic<uint32_t> sampleRate { AudioDevice::kSampleRate };
    std::atomic<float>    volume     { 0.5f };       // pre-mix gain
    std::atomic<bool>     muted      { false };

    /// RateAware override — auto-config when AudioDevice::addSource picks
    /// this AudioSrc up (see MainWindow plugMockingboard).
    void setSampleRate(uint32_t hz) override
    {
        if (hz == 0) hz = AudioDevice::kSampleRate;
        sampleRate.store(hz, std::memory_order_relaxed);
    }

    // Per-chip synthesis state. Counters are INTEGERS (MAME parity —
    // `ay8910.cpp:998-1015` uses uint counters). The fractional
    // accumulator captures the sub-tick remainder that builds up
    // between audio samples at the typical kAyToneStepHz/sampleRate
    // ratio (≈ 22.7 ticks/sample at 44.1 kHz). Pre-2026-05-16 POM2
    // used pure float counters which aliased noticeably at periods
    // 1-3 (PWM tricks like Cosmic Bouncer sounded sour) because
    // float arithmetic accumulated rounding error across the
    // `while (counter >= period)` resolve loop. Integer counters
    // remove that drift entirely.
    struct ChipState {
        // 3 tone channels: integer phase counter + fractional sub-tick
        // accumulator + current output bit.
        uint16_t toneCounter[3] = { 0, 0, 0 };
        float    toneAccum  [3] = { 0, 0, 0 };
        uint8_t  toneOut    [3] = { 0, 0, 0 };
        // Noise: 17-bit LFSR.
        // `noisePrescale` halves the LFSR update rate vs the noise
        // counter — MAME `ay8910.cpp:1086-1104` toggles `m_prescale_noise`
        // on every counter expiry and only calls `noise_rng_tick()` on
        // alternate cycles. Without this, after the tone-rate fix
        // (counter at clock/8 instead of clock/16) the LFSR would tick
        // 2× too fast, making noise hiss too coarse.
        //
        // `lastSeenResetCount` tracks `ayResetCount_[chip]` so we can
        // re-seed the LFSR to MAME's reset value (`m_rng = 1`, MAME
        // `ay8910.cpp:1309`) when the CPU side strobes PB2=0. Without
        // this re-seed, noise sequences are not deterministic across
        // resets (POM2's CPU-thread `Ay3_8910::reset()` clears regs but
        // can't reach this audio-thread state).
        uint16_t noiseCounter        = 0;
        float    noiseAccum          = 0;
        uint32_t noiseLfsr           = 1;       // MAME ay8910.cpp:1309
        uint8_t  noiseOut            = 0;
        uint8_t  noisePrescale       = 0;
        uint32_t lastSeenResetCount  = 0;
        // Envelope state machine. Verbatim port of MAME `ay8910.h:182-219`
        // + `ay8910.cpp:989-1020`. The 4 flags (attack, hold, alternate,
        // holding) are derived from R13 via `setShape`, called whenever
        // we detect R13 has changed. `step` walks 15→0 (down); when it
        // hits -1, hold/alternate decide the wrap behaviour. `volume =
        // step ^ attack` is the live 4-bit DAC level. Replaces the
        // earlier branchy `envStep` 0..31 model which mis-handled
        // shapes 10 (/\/\), 12 (\\\\), and 14 (\/\/) — vibrato patterns
        // used by Mockingboard music drivers.
        uint32_t envCounter     = 0;
        float    envAccum       = 0;
        int      envStep        = 15;    // walks 15 → 0
        uint8_t  envAttack      = 0;     // 0 or 15
        uint8_t  envHold        = 0;
        uint8_t  envAlternate   = 0;
        uint8_t  envHolding     = 0;
        int      lastShape      = -1;    // forces setShape on first sample
        // Tracks `ayEnvWriteCount_[chip]` so a write to R13 with an
        // UNCHANGED shape value still restarts the envelope (real AY-3-8910
        // behaviour — set_shape runs on every R13 store). The register
        // snapshot alone can't reveal a same-value store.
        uint32_t lastSeenEnvWriteCount = 0;
        bool     envRetrigger          = false;
    };
    ChipState chip[2];

    void fillAudioBuffer(float* output, int frameCount) override
    {
        if (frameCount <= 0) return;
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        if (sr == 0) { std::fill_n(output, frameCount, 0.0f); return; }
        const bool  isMuted = muted.load(std::memory_order_relaxed);
        const float vol     = volume.load(std::memory_order_relaxed);

        // Snapshot both chips' register banks under the parent mutex.
        // Brief: 32 bytes of memcpy. The CPU thread holds this same
        // mutex on every VIA write, so contention is bounded. Also
        // snapshot `ayResetCount_[]` so the audio thread can detect a
        // PB2=0 reset that fired in the inter-block window and re-seed
        // the noise LFSR per MAME (deterministic noise after reset).
        uint8_t  regSnap[2][kAyNumRegs];
        uint32_t resetCountSnap[2];
        uint32_t envWriteCountSnap[2];
        {
            std::lock_guard<std::mutex> lk(parent->mtx);
            std::memcpy(regSnap[0], parent->ay_[0]->regs, kAyNumRegs);
            std::memcpy(regSnap[1], parent->ay_[1]->regs, kAyNumRegs);
            resetCountSnap[0] = parent->ayResetCount_[0];
            resetCountSnap[1] = parent->ayResetCount_[1];
            envWriteCountSnap[0] = parent->ayEnvWriteCount_[0];
            envWriteCountSnap[1] = parent->ayEnvWriteCount_[1];
        }
        for (int ci = 0; ci < 2; ++ci) {
            if (chip[ci].lastSeenResetCount != resetCountSnap[ci]) {
                chip[ci].lastSeenResetCount = resetCountSnap[ci];
                chip[ci].noiseLfsr     = 1;   // MAME ay8910.cpp:1309
                chip[ci].noisePrescale = 0;
                chip[ci].noiseOut      = 0;
            }
            // A write to R13 (even same value) restarts the envelope.
            if (chip[ci].lastSeenEnvWriteCount != envWriteCountSnap[ci]) {
                chip[ci].lastSeenEnvWriteCount = envWriteCountSnap[ci];
                chip[ci].envRetrigger = true;
            }
        }

        // Pre-compute per-sample increments. AY internal step rates are
        // fixed (function of pin-22 clock and divider); only the periods
        // vary with the registers, so we recompute periods inside the
        // loop.
        const float toneStepPerSample  = kAyToneStepHz  / static_cast<float>(sr);
        const float noiseStepPerSample = kAyNoiseStepHz / static_cast<float>(sr);
        // Envelope step-per-sample (kAyEnvStepHz / sr) is computed inline
        // when the AY envelope branch fires — leaving the pre-computed
        // local here would just warn unused under -Wall.

        if (isMuted) {
            std::fill_n(output, frameCount, 0.0f);
            return;
        }

        for (int i = 0; i < frameCount; ++i) {
            float sample = 0.0f;
            for (int ci = 0; ci < 2; ++ci) {
                ChipState& cs = chip[ci];
                const uint8_t* r = regSnap[ci];

                // ── Per-channel tone period (R0/R1 pair, etc.). ────
                // 12-bit period; period 0 is treated as 1 by real
                // hardware (channel always-on at audio rate).
                const auto tonePeriod = [&](int ch) -> uint16_t {
                    const int lo = r[ch * 2];
                    const int hi = r[ch * 2 + 1] & 0x0F;
                    const int p  = (hi << 8) | lo;
                    return static_cast<uint16_t>(p == 0 ? 1 : p);
                };

                // Step tone counters in integer ticks (MAME parity —
                // `ay8910.cpp:998-1015` uses uint counters). The float
                // accumulator handles the fractional sub-tick rate at
                // the audio output rate; each integer tick increments
                // the counter and toggles output when ≥ period. Full
                // cycle = 2 × period (AY T-flop divides by 2).
                for (int ch = 0; ch < 3; ++ch) {
                    cs.toneAccum[ch] += toneStepPerSample;
                    const uint16_t p = tonePeriod(ch);
                    while (cs.toneAccum[ch] >= 1.0f) {
                        cs.toneAccum[ch] -= 1.0f;
                        if (++cs.toneCounter[ch] >= p) {
                            cs.toneCounter[ch] = 0;
                            cs.toneOut[ch] ^= 1;
                        }
                    }
                }

                // ── Noise ────────────────────────────────────────
                // 5-bit period in R6. MAME `ay8910.cpp:1086-1104`
                // toggles `m_prescale_noise ^= 1` on every counter
                // expiry and only ticks the LFSR when prescale lands
                // on 0 → effective LFSR rate = clock/(16*NP). Integer
                // counter (MAME parity).
                const uint16_t noisePer = static_cast<uint16_t>(
                    (r[6] & 0x1F) ? (r[6] & 0x1F) : 1);
                cs.noiseAccum += noiseStepPerSample;
                while (cs.noiseAccum >= 1.0f) {
                    cs.noiseAccum -= 1.0f;
                    if (++cs.noiseCounter < noisePer) continue;
                    cs.noiseCounter = 0;
                    cs.noisePrescale ^= 1;
                    if (cs.noisePrescale) continue;
                    // 17-bit LFSR, polynomial x^17 + x^14 + 1. (MAME's
                    // canonical AY noise tap pair.)
                    const uint32_t bit =
                        ((cs.noiseLfsr >> 0) ^ (cs.noiseLfsr >> 3)) & 1;
                    cs.noiseLfsr = (cs.noiseLfsr >> 1) | (bit << 16);
                    cs.noiseOut  = static_cast<uint8_t>(cs.noiseLfsr & 1);
                }

                // ── Envelope (MAME-verbatim 4-flag state machine) ─
                // R13 = shape register, 4 bits.  On every R13 write
                // MAME's `set_shape` reinitialises the machine; we
                // detect the write by comparing against the cached
                // `lastShape`. Period = R11 + (R12 << 8). Counter ticks
                // at clock/8 (same as tone); MAME `ay8910.cpp:994`:
                // `period = envelope->period * m_step` with `m_step=2`
                // for AY-3-8910, so each step takes `2*envPer` ticks
                // at clock/8 → individual step rate = clock/(16*envPer).
                // A full 16-step ramp therefore completes in
                // 32*envPer/(clock/8) = 256*envPer/clock seconds — full
                // cycle freq = clock/(256*envPer), matches datasheet.
                {
                    const int shape = r[13] & 0x0F;
                    if (shape != cs.lastShape || cs.envRetrigger) {
                        cs.envRetrigger = false;   // consumed
                        // MAME `ay8910.h:204-219` set_shape:
                        //   attack = (shape & 0x04) ? mask : 0
                        //   if (!(shape & 0x08))  // continue == 0
                        //       hold = 1; alternate = attack;
                        //   else
                        //       hold      = shape & 0x01;
                        //       alternate = shape & 0x02;
                        //   step = mask; holding = 0;
                        constexpr uint8_t kMask = 0x0F;
                        cs.envAttack = (shape & 0x04) ? kMask : uint8_t{0};
                        if ((shape & 0x08) == 0) {
                            cs.envHold      = 1;
                            cs.envAlternate = cs.envAttack;
                        } else {
                            cs.envHold      = (shape & 0x01) ? 1 : 0;
                            cs.envAlternate = (shape & 0x02) ? 1 : 0;
                        }
                        cs.envStep      = kMask;
                        cs.envHolding   = 0;
                        cs.envCounter   = 0;
                        cs.envAccum     = 0;
                        cs.lastShape    = shape;
                    }
                    const int envPer = (r[11] | (r[12] << 8))
                                       ? (r[11] | (r[12] << 8)) : 1;
                    // MAME `ay8910.cpp:994`: `period = envelope->period * m_step`
                    // where `m_step = 2` for AY-3-8910. Each envelope
                    // step takes `envPer * 2` base ticks; the base
                    // rate is the same clock/8 we step the tone with.
                    // Integer counter (MAME parity).
                    const uint32_t threshold =
                        static_cast<uint32_t>(envPer) * 2u;
                    cs.envAccum += toneStepPerSample;
                    while (cs.envAccum >= 1.0f) {
                        cs.envAccum -= 1.0f;
                        if (++cs.envCounter < threshold) continue;
                        cs.envCounter = 0;
                        if (cs.envHolding) continue;
                        cs.envStep--;
                        if (cs.envStep < 0) {
                            // MAME `ay8910.cpp:1000-1015` end-of-ramp:
                            //   if (hold) {
                            //       if (alternate) attack ^= mask;
                            //       holding = 1; step = 0;
                            //   } else {
                            //       if (alternate && (step & (mask+1)))
                            //           attack ^= mask;
                            //       step &= mask;
                            //   }
                            constexpr uint8_t kMask = 0x0F;
                            if (cs.envHold) {
                                if (cs.envAlternate) cs.envAttack ^= kMask;
                                cs.envHolding = 1;
                                cs.envStep    = 0;
                            } else {
                                if (cs.envAlternate
                                    && (cs.envStep & (kMask + 1))) {
                                    cs.envAttack ^= kMask;
                                }
                                cs.envStep &= kMask;
                            }
                        }
                    }
                }
                // Output level: `volume = step ^ attack` (MAME
                // `ay8910.cpp:1020`). step ∈ [0..15], attack ∈ {0, 15}.
                const uint8_t envOut = static_cast<uint8_t>(
                    cs.envStep ^ cs.envAttack);

                // ── Mixer (R7) ────────────────────────────────────
                // R7 bit n (n=0..2) = tone-disable for channel n
                // (active low). Bit n+3 = noise-disable for channel n.
                // Mockingboard convention: AY output = sum of channel
                // outputs, where each channel = (tone_out | tone_dis)
                // AND (noise_out | noise_dis), gated by per-channel
                // amplitude (R8/R9/R10).
                const uint8_t mix = r[7];
                for (int ch = 0; ch < 3; ++ch) {
                    const bool toneEn  = ((mix >> ch)       & 1) == 0;
                    const bool noiseEn = ((mix >> (ch + 3)) & 1) == 0;
                    const uint8_t chOut =
                        (toneEn  ? cs.toneOut[ch] : 1) &
                        (noiseEn ? cs.noiseOut    : 1);
                    if (!chOut) continue;
                    const uint8_t ampReg = r[8 + ch];
                    const uint8_t level =
                        (ampReg & 0x10) ? envOut
                                        : static_cast<uint8_t>(ampReg & 0x0F);
                    sample += kAyVolumeTable[level & 0x0F];
                }
            }
            // Each AY peaks at ~3.0 (3 channels × 1.0). Two AYs sum to
            // 6.0. Pre-divide by 6 so a maxed-out signal sits at 1.0
            // before the volume knob; mixer clamp stops anything past.
            output[i] = (sample / 6.0f) * vol;
        }
    }
};

// ─── MockingboardCard ─────────────────────────────────────────────────────

MockingboardCard::MockingboardCard(int slotNum)
    : slot_(slotNum)
{
    via_[0] = std::make_unique<Via6522>();
    via_[1] = std::make_unique<Via6522>();
    ay_[0]  = std::make_unique<Ay3_8910>();
    ay_[1]  = std::make_unique<Ay3_8910>();
    audio_  = std::make_unique<AudioSrc>(this);
    onReset();
}

MockingboardCard::~MockingboardCard() = default;

void MockingboardCard::onUnplug()
{
    // SlotBus::detachFromBus() auto-releases any pending IRQ line bit
    // before letting us go, so no explicit assertIrq(false) here.
}

void MockingboardCard::onReset()
{
    std::lock_guard<std::mutex> lk(mtx);
    via_[0]->reset();
    via_[1]->reset();
    ay_[0]->reset();
    ay_[1]->reset();
    assertIrq(false);
    // Re-anchor the lazy-sync clock to "now" so a freshly reset card
    // doesn't run a giant catch-up on its first MMIO access.
    lastSyncCycle_ = cpu_ ? cpu_->getCycleCountNow() : 0;
    viaWriteCount_[0] = viaWriteCount_[1] = 0;
    ayWriteCount_[0]  = ayWriteCount_[1]  = 0;
    ayResetCount_[0]  = ayResetCount_[1]  = 0;
    ayEnvWriteCount_[0] = ayEnvWriteCount_[1] = 0;
}

AudioSource* MockingboardCard::audioSource()
{
    return audio_.get();
}

void MockingboardCard::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = AudioDevice::kSampleRate;
    audio_->sampleRate.store(hz, std::memory_order_relaxed);
}

void MockingboardCard::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    audio_->volume.store(v, std::memory_order_relaxed);
}
float MockingboardCard::getVolume() const
{
    return audio_->volume.load(std::memory_order_relaxed);
}
void MockingboardCard::setMuted(bool m)
{
    audio_->muted.store(m, std::memory_order_relaxed);
}
bool MockingboardCard::isMuted() const
{
    return audio_->muted.load(std::memory_order_relaxed);
}

// Lazy timer catch-up. The Mockingboard's 6522 VIA T1/T2 counters tick
// once per CPU cycle on real hardware. POM2's host loop advances slot
// peripherals in batches at the end of each CPU run-slice (~17 045
// cycles in default mode), which is fine for steady-state music but
// breaks the tight write-T1-then-read-IFR sequences detection routines
// rely on (Nox Archaist, Skyfox, Broadside — see CLAUDE.md). Sync the
// VIAs to "now" before every MMIO access so the IFR the routine reads
// reflects the cycles that have actually elapsed since its T1 write.
// Caller must hold `mtx`.
void MockingboardCard::syncToCpuCycle()
{
    if (!cpu_) return;
    const uint64_t now = cpu_->getCycleCountNow();
    if (now <= lastSyncCycle_) {
        lastSyncCycle_ = now;
        return;
    }
    const uint64_t delta = now - lastSyncCycle_;
    // The VIAs' `advance()` takes an int; clamp to a sane upper bound
    // (a single CPU run-slice is ~17 045 cycles, so anything beyond a
    // few million here means our sync clock got desynchronised).
    const int step = (delta > 0x7FFFFFFFu) ? 0x7FFFFFFF
                                           : static_cast<int>(delta);
    via_[0]->advance(step);
    via_[1]->advance(step);
    lastSyncCycle_ = now;
}

uint8_t MockingboardCard::slotRomRead(uint8_t low8)
{
    // Address decode: bit 7 selects which VIA, bits 0..3 select the
    // register inside the VIA. Bits 4..6 are mirrors (real hardware
    // partial-decodes the same way — the chip has only A0..A3 wired).
    std::lock_guard<std::mutex> lk(mtx);
    syncToCpuCycle();     // make T1/T2/IFR cycle-accurate at "now"
    const int chip = (low8 & 0x80) ? 1 : 0;
    const uint8_t out = via_[chip]->read(low8 & 0x0F);
    updateIrq();          // T1CL clears IFR.T1, may drop IRQ
    return out;
}

void MockingboardCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx);
    syncToCpuCycle();     // T1 counters reflect "now" before T1CH reload
    const int chip = (low8 & 0x80) ? 1 : 0;
    ++viaWriteCount_[chip];
    const uint8_t events = via_[chip]->write(low8 & 0x0F, v);
    if (events) onViaPortBChange(chip);
    updateIrq();
}

uint32_t MockingboardCard::getAyCommandCount(int chip, int cmd) const
{
    if (chip < 0 || chip > 1 || cmd < 0 || cmd > 3) return 0;
    const auto& ay = *ay_[chip];
    switch (cmd) {
        case 0: return ay.inactiveCount;
        case 1: return ay.readStrobeCount;
        case 2: return ay.writeStrobeCount;
        case 3: return ay.latchCount;
    }
    return 0;
}

void MockingboardCard::onViaPortBChange(int chip)
{
    // Marshal the VIA's current Port A / Port B output to the AY.
    // Note this fires for *both* PA and PB changes (events bit 0/1) —
    // PA-only changes also matter because LATCH/WRITE strobes bring PA
    // (the AY data bus) to the chip just before / just after PB sets
    // BDIR/BC1. We reapply on either edge so the order doesn't matter.
    const uint8_t pa = via_[chip]->portAOut & via_[chip]->ddrA;
    const uint8_t pb = via_[chip]->portBOut & via_[chip]->ddrB;
    const auto res = ay_[chip]->applyControl(pa, pb);
    if (res == Ay3_8910::ApplyResult::Wrote) {
        ++ayWriteCount_[chip];
        // R13 (envelope shape) restarts the envelope generator on EVERY
        // write, even when the value is unchanged. Surface same-value R13
        // stores to the audio thread (which only sees the register snapshot)
        // as a monotonic counter, mirroring the ayResetCount_ pattern.
        if ((ay_[chip]->latchedAddr & 0x0F) == 13) ++ayEnvWriteCount_[chip];
    } else if (res == Ay3_8910::ApplyResult::ResetOnly) {
        ++ayResetCount_[chip];
    }
}

void MockingboardCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    std::lock_guard<std::mutex> lk(mtx);
    if (cpu_) {
        // Lazy-sync path: any cycles already accounted for via MMIO
        // accesses during this slice are skipped — `syncToCpuCycle()`
        // advanced the VIAs to that point already. Just catch up the
        // remainder so end-of-slice IRQ state is published.
        syncToCpuCycle();
    } else {
        // No CPU back-pointer (unit-test harness — see
        // mockingboard_smoke_test.cpp). Fall back to the legacy
        // batched advance so existing tests keep their semantics.
        via_[0]->advance(cycles);
        via_[1]->advance(cycles);
    }
    updateIrq();
}

void MockingboardCard::updateIrq()
{
    const bool combined = via_[0]->irqOut() || via_[1]->irqOut();
    // `assertIrq()` debounces against the base-class cache and fans out
    // through the SlotBus IRQ router (installed by Memory::setCpu).
    // Wire-OR semantics on the CPU side mean another card on a different
    // slot stays asserted even when this one releases — see
    // M6502::setIrqLine().
    assertIrq(combined);
}

uint8_t MockingboardCard::getAyRegister(int chip, int reg) const
{
    if (chip < 0 || chip > 1 || reg < 0 || reg >= kAyNumRegs) return 0;
    std::lock_guard<std::mutex> lk(mtx);
    return ay_[chip]->regs[reg];
}

uint8_t MockingboardCard::peekViaRegister(int chip, int reg) const
{
    if (chip < 0 || chip > 1 || reg < 0 || reg > 15) return 0xFF;
    std::lock_guard<std::mutex> lk(mtx);
    // Read-only peek: replicate the read() switch but skip side effects.
    auto& v = *via_[chip];
    switch (reg & 0x0F) {
    case VIA_ORB:    return v.readPortB();
    case VIA_ORA:    return v.readPortA();
    case VIA_DDRB:   return v.ddrB;
    case VIA_DDRA:   return v.ddrA;
    case VIA_T1CL:   return static_cast<uint8_t>(v.t1Counter & 0xFF);
    case VIA_T1CH:   return static_cast<uint8_t>((v.t1Counter >> 8) & 0xFF);
    case VIA_T1LL:   return static_cast<uint8_t>(v.t1Latch & 0xFF);
    case VIA_T1LH:   return static_cast<uint8_t>((v.t1Latch >> 8) & 0xFF);
    case VIA_T2CL:   return static_cast<uint8_t>(v.t2Counter & 0xFF);
    case VIA_T2CH:   return static_cast<uint8_t>((v.t2Counter >> 8) & 0xFF);
    case VIA_SR:     return v.sr;
    case VIA_ACR:    return v.acr;
    case VIA_PCR:    return v.pcr;
    case VIA_IFR:    return v.computedIfr();
    case VIA_IER:    return static_cast<uint8_t>(v.ier | 0x80);
    case VIA_ORANH:  return v.readPortA();
    default:         return 0xFF;
    }
}

// ── Note on the parent mutex ─────────────────────────────────────────────
//
// The MockingboardCard's `mtx` is referenced from the AudioSrc inner class
// (declared in the header as `std::mutex` member of the card). It guards:
//   * the VIA register file (read/write/advance)
//   * the AY register banks (writes from the CPU side, snapshot reads from
//     the audio thread)
// The AudioSrc holds the lock only briefly (32-byte memcpy) per audio
// callback — it does NOT hold it during the synthesis loop. The CPU side
// holds it for the duration of one slotRomRead/Write or advanceCycles()
// call, which is bounded.

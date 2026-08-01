// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AyPsgSynth — the audio-thread synthesis core shared by every card that
// carries AY-3-8910/8913 PSGs: MockingboardCard (2 chips), PhasorCard
// (4 chips), and any future board. `Ay3_8910.h` owns the CPU-side
// register bank and the VIA control-bus decoder; THIS header owns what
// happens on the audio thread — tone/noise/envelope generators, the
// mixer, and the band-limiting that turns their 1-bit outputs into
// samples.
//
// Extracted 2026-08-01. Until then MockingboardCard and PhasorCard each
// carried a private, verbatim-duplicated copy (~130 lines, 4 differing
// lines) and had already drifted apart: Phasor never gained the
// cycle-stamped event queue, so every register write inside an audio
// buffer collapsed to its last value. Duplicated synthesis meant every
// audio fix had to be applied twice or one card silently regressed.
//
// ─── Rate model ──────────────────────────────────────────────────────
//
// The AY's pin-22 CLOCK is wired to the slot's phase-0 line, so the chip
// runs at the CPU clock — 1 022 727 Hz on NTSC, 1 015 625 Hz on PAL.
// Tone, noise and envelope all derive from ONE base tick at clock/8
// (~127.8 kHz); MAME allocates a single stream at exactly that rate
// (`ay8910.cpp:1298`, `stream_alloc(0, m_streams, master_clock / 8)`)
// and advances all three counters together. The datasheet's
// clock/(16*TP) tone formula comes out of a clock/8 counter because the
// AY has a divide-by-2 output T-flop, which is the `^= 1` toggle here.
//
// ─── Band-limiting ───────────────────────────────────────────────────
//
// MAME renders on the chip's clock/8 grid and hands the result to a
// decimating resampler (`src/emu/resampler.cpp`). POM2 renders straight
// to the device rate, so the decimation has to happen inline:
// `renderChipSample` BOX-INTEGRATES the mixer across the ~2.9 base ticks
// that one output sample spans, weighting the partial ticks at each end
// by their true duration.
//
// That is not a cosmetic refinement. The previous code advanced the same
// counters in the same integer ticks and then point-sampled the mixer
// once per output sample, discarding the sub-sample edge position it had
// just computed. Two consequences, both measured:
//
//   * Every square-wave edge snapped to the output grid (+/-22.7 us of
//     jitter at 44.1 kHz) and everything above Nyquist folded back into
//     the audible band — 7 % of total output power was inharmonic on an
//     ordinary 4 kHz note. With box integration that falls to 0.5 %.
//     (`tests/mockingboard_audio_quality_test.cpp`, test 1.)
//   * At envelope periods below 2, whole envelope steps were never
//     sampled at all.
//
// ─── DC ──────────────────────────────────────────────────────────────
//
// The channel model is unipolar — a channel contributes `table[level]`
// or nothing — so a 50 %-duty tone carries a DC term of half its
// amplitude, and a channel with tone AND noise masked off in R7 (the
// volume-register PWM / digi technique) is pure DC. Every note and every
// volume write then steps the DC level, which is an audible click, and
// half the headroom is spent on an inaudible offset. Real hardware
// AC-couples through the card's output capacitor; MAME high-passes each
// speaker at 20 Hz by default (`src/emu/audio_effects/filter.cpp:39-44`
// highpass active, `:63-68` fh = 20 Hz). `AyDcBlocker` is the 1-pole
// equivalent, the same idiom POM2 already uses for the speaker
// (`SpeakerDevice.cpp:193-197`).

#ifndef POM2_AY_PSG_SYNTH_H
#define POM2_AY_PSG_SYNTH_H

#include "Ay3_8910.h"

#include <cmath>
#include <cstdint>

namespace pom2 {
namespace ay {

/// Logarithmic 4-bit volume → linear amplitude. Westcott's 2001 measured
/// AY-3-8910 output voltages (the dataset MAME carries as `ay8910_param`,
/// `ay8910.cpp:695-712`), renormalised to 0..1. Index 0 is silence, index
/// 15 the per-channel peak, so three channels at peak put one AY at 3.0.
///
/// NOT MAME's `build_single_table` output, despite what the copies in
/// Mockingboard.cpp / PhasorCard.cpp used to claim: MAME's normalize = 1
/// branch maps to [-0.125, +0.375] (deliberately DC-offset, because MAME
/// then high-passes it), and normalize is only 1 under
/// AY8910_LEGACY_OUTPUT; the Mockingboard's AY8913 with 3 streams takes
/// normalize = 0 and gets raw divider ratios. The underlying measurements
/// agree within a few percent. Citation corrected 2026-08-01.
inline constexpr float kVolumeTable[16] = {
    0.0000f, 0.0105f, 0.0154f, 0.0223f, 0.0321f, 0.0468f, 0.0635f, 0.1061f,
    0.1319f, 0.2164f, 0.2974f, 0.3909f, 0.5128f, 0.6371f, 0.8186f, 1.0000f
};

inline constexpr int kNumRegs = Ay3_8910::kAyNumRegs;

/// One PSG's audio-thread synthesis state. Counters are INTEGERS (MAME
/// parity — `ay8910.cpp:998-1015`); `tickPhase` carries the fractional
/// position between clock/8 ticks so the render loop can integrate.
///
/// Audio-thread-only: never snapshotted, never touched by the CPU side.
struct ChipSynthState {
    /// Position inside the current clock/8 tick, in tick units [0,1).
    /// Shared by tone, noise and envelope — they run off the same base
    /// tick, so one phase serves all three.
    float    tickPhase       = 0.0f;

    uint16_t toneCounter[3]  = { 0, 0, 0 };
    uint8_t  toneOut    [3]  = { 0, 0, 0 };

    uint16_t noiseCounter    = 0;
    uint32_t noiseLfsr       = 1;      // MAME ay8910.cpp:1309 reset seed
    uint8_t  noiseOut        = 0;
    uint8_t  noisePrescale   = 0;

    // Envelope state machine — verbatim port of MAME `ay8910.h:243-259`
    // (set_shape) + `ay8910.cpp:1113-1147` (step + ramp end). `envStep`
    // walks 15 → 0; on reaching -1, hold/alternate decide the wrap.
    // `volume = step ^ attack` is the live 4-bit DAC level.
    uint32_t envCounter      = 0;
    int      envStep         = 15;
    uint8_t  envAttack       = 0;      // 0 or 15
    uint8_t  envHold         = 0;
    uint8_t  envAlternate    = 0;
    uint8_t  envHolding      = 0;
    int      lastShape       = -1;     // forces applyEnvShape on sample 1
    bool     envRetrigger    = false;

    // Card-side bookkeeping the audio thread compares against, so a
    // wholesale CPU-side reset or a same-value R13 store is visible here.
    uint32_t lastSeenResetCount    = 0;
    uint32_t lastSeenEnvWriteCount = 0;

    /// Re-seed the generators the way MAME's `ay8910_reset_ym` does
    /// (`ay8910.cpp:1309-1319`) after a PB2=0 strobe zeroes the bank.
    void resetGenerators()
    {
        noiseLfsr     = 1;
        noisePrescale = 0;
        noiseOut      = 0;
    }
};

/// R13 (shape) re-initialisation — MAME `ay8910.h:243-259 set_shape`,
/// which runs on EVERY R13 store, including a store of the value the
/// register already holds. `envRetrigger` (fed from the card's
/// `ayEnvWriteCount_`) is how a same-value store reaches us, since the
/// register snapshot alone cannot reveal one.
inline void applyEnvShape(ChipSynthState& cs, const uint8_t* r)
{
    const int shape = r[13] & 0x0F;
    if (shape == cs.lastShape && !cs.envRetrigger) return;
    cs.envRetrigger = false;                       // consumed
    constexpr uint8_t kMask = 0x0F;
    cs.envAttack = (shape & 0x04) ? kMask : uint8_t{0};
    if ((shape & 0x08) == 0) {
        // Continue = 0: map to the equivalent Continue = 1 shape.
        cs.envHold      = 1;
        cs.envAlternate = cs.envAttack;
    } else {
        cs.envHold      = (shape & 0x01) ? 1 : 0;
        cs.envAlternate = (shape & 0x02) ? 1 : 0;
    }
    cs.envStep    = kMask;
    cs.envHolding = 0;
    cs.envCounter = 0;
    cs.lastShape  = shape;
}

/// Advance one PSG by exactly ONE clock/8 base tick — the grid MAME's
/// stream runs on (`ay8910.cpp:1071-1147`).
inline void stepTick(ChipSynthState& cs, const uint8_t* r)
{
    // ── Tone (MAME `ay8910.cpp:1073-1084`) ──
    // 12-bit period; period 0 behaves as 1 (MAME clamps identically,
    // `std::max<int>(1, tone->period)` at `:1077`). The `^= 1` toggle is
    // the AY's divide-by-2 output T-flop.
    for (int ch = 0; ch < 3; ++ch) {
        const int pv = ((r[ch * 2 + 1] & 0x0F) << 8) | r[ch * 2];
        const uint16_t p = static_cast<uint16_t>(pv == 0 ? 1 : pv);
        if (++cs.toneCounter[ch] >= p) {
            cs.toneCounter[ch] = 0;
            cs.toneOut[ch] ^= 1;
        }
    }
    // ── Noise (MAME `ay8910.cpp:1086-1105`) ──
    // `noisePrescale` is MAME's `m_prescale_noise`: it toggles on every
    // counter expiry and the LFSR ticks only on alternate expiries,
    // making the effective LFSR rate clock/(16*NP).
    const uint16_t noisePer =
        static_cast<uint16_t>((r[6] & 0x1F) ? (r[6] & 0x1F) : 1);
    if (++cs.noiseCounter >= noisePer) {
        cs.noiseCounter = 0;
        cs.noisePrescale ^= 1;
        if (!cs.noisePrescale) {
            // 17-bit LFSR, x^17 + x^14 + 1 (MAME `ay8910.h:263-273`).
            const uint32_t bit = ((cs.noiseLfsr >> 0) ^ (cs.noiseLfsr >> 3)) & 1;
            cs.noiseLfsr = (cs.noiseLfsr >> 1) | (bit << 16);
            cs.noiseOut  = static_cast<uint8_t>(cs.noiseLfsr & 1);
        }
    }
    // ── Envelope (MAME `ay8910.cpp:1113-1147`) ──
    // MAME wraps the WHOLE counter update in `if (holding == 0)`
    // (`:1117-1119`), so a held envelope freezes its counter too.
    if (!cs.envHolding) {
        // Period = R11 | R12<<8, times MAME's `m_step` = 2 for
        // PSG_TYPE_AY (`ay8910.cpp:1576`; `period = envelope->period *
        // m_step` at `:1119`). Deliberately NOT clamped at 0:
        // `set_period` has no clamp, so EP=0 steps every base tick,
        // exactly double the EP=1 rate.
        const uint32_t threshold =
            static_cast<uint32_t>(r[11] | (r[12] << 8)) * 2u;
        if (++cs.envCounter >= threshold) {
            cs.envCounter = 0;
            cs.envStep--;
            if (cs.envStep < 0) {
                constexpr uint8_t kMask = 0x0F;
                if (cs.envHold) {
                    if (cs.envAlternate) cs.envAttack ^= kMask;
                    cs.envHolding = 1;
                    cs.envStep    = 0;
                } else {
                    // step is -1 here; -1 & 0x10 == 0x10 in C++, the same
                    // integer promotion MAME's `s8 step` gets.
                    if (cs.envAlternate && (cs.envStep & (kMask + 1)))
                        cs.envAttack ^= kMask;
                    cs.envStep &= kMask;
                }
            }
        }
    }
}

/// Mixed analogue level of one PSG at its CURRENT tick state, in units
/// where one channel at volume 15 contributes 1.0.
inline float chipLevel(const ChipSynthState& cs, const uint8_t* r)
{
    // `volume = step ^ attack` (MAME `ay8910.cpp:1020`).
    const uint8_t envOut = static_cast<uint8_t>(cs.envStep ^ cs.envAttack);
    // R7 bit n (n=0..2) = tone-disable for channel n (active low); bit
    // n+3 = noise-disable. Channel = (tone_out | tone_dis) AND
    // (noise_out | noise_dis), gated by amplitude R8/R9/R10.
    const uint8_t mix = r[7];
    float level = 0.0f;
    for (int ch = 0; ch < 3; ++ch) {
        const bool toneEn  = ((mix >> ch)       & 1) == 0;
        const bool noiseEn = ((mix >> (ch + 3)) & 1) == 0;
        const uint8_t chOut = (toneEn  ? cs.toneOut[ch] : uint8_t{1}) &
                              (noiseEn ? cs.noiseOut    : uint8_t{1});
        if (!chOut) continue;
        const uint8_t ampReg = r[8 + ch];
        const uint8_t lv = (ampReg & 0x10)
                               ? envOut
                               : static_cast<uint8_t>(ampReg & 0x0F);
        level += kVolumeTable[lv & 0x0F];
    }
    return level;
}

/// Render ONE output sample for one PSG, box-integrating the mixer over
/// the `ticksPerSample` base ticks the sample spans. See the header
/// comment for why this and not a point sample.
///
/// `invTicksPerSample` is passed in rather than derived so the caller can
/// hoist the reciprocal out of its inner loop.
inline float renderChipSample(ChipSynthState& cs, const uint8_t* r,
                              float ticksPerSample, float invTicksPerSample)
{
    applyEnvShape(cs, r);
    float acc       = 0.0f;
    float remaining = ticksPerSample;
    while (remaining > 0.0f) {
        const float avail = 1.0f - cs.tickPhase;
        if (avail <= 0.0f) {          // fp equality guard, not a hot path
            cs.tickPhase = 0.0f;
            stepTick(cs, r);
            continue;
        }
        const float dt = (avail < remaining) ? avail : remaining;
        acc          += chipLevel(cs, r) * dt;
        cs.tickPhase += dt;
        remaining    -= dt;
        if (cs.tickPhase >= 1.0f) {
            cs.tickPhase = 0.0f;
            stepTick(cs, r);
        }
    }
    return acc * invTicksPerSample;
}

/// One-pole DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1], with the pole
/// placed at MAME's default 20 Hz speaker high-pass corner.
struct DcBlocker {
    static constexpr float kCornerHz = 20.0f;

    void setRate(uint32_t sr)
    {
        if (sr == rate_ || sr == 0) return;
        rate_ = sr;
        pole_ = std::exp(-2.0f * 3.14159265358979f * kCornerHz
                         / static_cast<float>(sr));
    }

    float process(float x)
    {
        const float y = x - prevX_ + pole_ * prevY_;
        prevX_ = x;
        prevY_ = y;
        return y;
    }

    void reset() { prevX_ = prevY_ = 0.0f; }

private:
    uint32_t rate_  = 0;
    float    pole_  = 0.0f;
    float    prevX_ = 0.0f;
    float    prevY_ = 0.0f;
};

}  // namespace ay
}  // namespace pom2

#endif  // POM2_AY_PSG_SYNTH_H

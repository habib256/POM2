// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Mockingboard.h"

#include "ByteIO.h"
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

// Convenience aliases — the VIA register layout, IFR bits, AY register
// count, and PB control-bus map all live as static members of the
// shared `pom2::Via6522` / `pom2::Ay3_8910` types since 2026-05-27.
// These using-declarations preserve the existing call sites without
// touching the (already-tested) implementation below.
using Via    = pom2::Via6522;
using AyChip = pom2::Ay3_8910;
constexpr int     kAyNumRegs    = AyChip::kAyNumRegs;
constexpr uint8_t kPbBitBc1     = AyChip::kPbBitBc1;
constexpr uint8_t kPbBitBdir    = AyChip::kPbBitBdir;
constexpr uint8_t kPbBitReset   = AyChip::kPbBitReset;
constexpr uint8_t IFR_T1        = Via::IFR_T1;
constexpr uint8_t IFR_T2        = Via::IFR_T2;
constexpr uint8_t IFR_ANY       = Via::IFR_ANY;
constexpr uint8_t VIA_ORB       = Via::VIA_ORB;
constexpr uint8_t VIA_ORA       = Via::VIA_ORA;
constexpr uint8_t VIA_DDRB      = Via::VIA_DDRB;
constexpr uint8_t VIA_DDRA      = Via::VIA_DDRA;
constexpr uint8_t VIA_T1CL      = Via::VIA_T1CL;
constexpr uint8_t VIA_T1CH      = Via::VIA_T1CH;
constexpr uint8_t VIA_T1LL      = Via::VIA_T1LL;
constexpr uint8_t VIA_T1LH      = Via::VIA_T1LH;
constexpr uint8_t VIA_T2CL      = Via::VIA_T2CL;
constexpr uint8_t VIA_T2CH      = Via::VIA_T2CH;
constexpr uint8_t VIA_SR        = Via::VIA_SR;
constexpr uint8_t VIA_ACR       = Via::VIA_ACR;
constexpr uint8_t VIA_PCR       = Via::VIA_PCR;
constexpr uint8_t VIA_IFR       = Via::VIA_IFR;
constexpr uint8_t VIA_IER       = Via::VIA_IER;
constexpr uint8_t VIA_ORANH     = Via::VIA_ORANH;

}  // namespace

// ─── Forward types ───────────────────────────────────────────────────────
//
// Via6522 + Ay3_8910 live in shared headers (`Via6522.h` / `Ay3_8910.h`)
// since 2026-05-27 so PhasorCard can reuse them verbatim — same VIA
// timer logic, same AY register-bank + control-bus decoder. AudioSrc
// stays private to this card; its 2-AY synthesis state is tightly
// coupled to MockingboardCard's `ayResetCount_` / `ayEnvWriteCount_`
// telemetry and the audio thread's mutex protocol. Phasor ships its
// own AudioSrc until/unless we extract a shared multi-AY synth class.

// File-scope aliases so call sites below (constructors,
// `via_[chip]->advance()`, `Ay3_8910::ApplyResult::Wrote`, …) stay
// unchanged after the extraction.
using Via6522  = pom2::Via6522;
using Ay3_8910 = pom2::Ay3_8910;


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

MockingboardCard::MockingboardCard(int slotNum, Variant variant)
    : slot_(slotNum), variant_(variant)
{
    via_[0] = std::make_unique<Via6522>();
    via_[1] = std::make_unique<Via6522>();
    ay_[0]  = std::make_unique<Ay3_8910>();
    ay_[1]  = std::make_unique<Ay3_8910>();
    if (variant_ == Variant::SoundII) {
        ssi_ = std::make_unique<pom2::Ssi263>();
    }
    audio_  = std::make_unique<AudioSrc>(this);
    onReset();
}

MockingboardCard::~MockingboardCard() = default;

void MockingboardCard::onUnplug()
{
    // SlotBus::detachFromBus() auto-releases any pending IRQ line bit
    // before letting us go, so no explicit assertIrq(false) here.
}

// ─── Rewind / snapshot ──────────────────────────────────────────────────────
// VIA + AY register/timer state — the audible music state of the card. The
// SSI263 speech chip's phoneme-playback position is NOT captured (speech
// across a rewind is rare; its A/!R→VIA1.CA1 IRQ latch IS restored via the
// VIA's ifr). Blob is self-describing (magic + version + present mask) so a
// foreign card on this slot ignores it. Both Mockingboard and Phasor reuse
// Via6522/Ay3_8910::append/loadSnapshot.
void MockingboardCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    using namespace pom2::byteio;
    std::lock_guard<std::mutex> lk(mtx);
    putU8(out, 'M'); putU8(out, 'B'); putU8(out, 'S'); putU8(out, 1);  // magic + ver
    putU8(out, static_cast<uint8_t>(variant_));
    uint8_t present = 0;
    if (via_[0]) present |= 0x01;
    if (via_[1]) present |= 0x02;
    if (ay_[0])  present |= 0x04;
    if (ay_[1])  present |= 0x08;
    if (ssi_)    present |= 0x10;
    putU8(out, present);
    if (via_[0]) via_[0]->appendSnapshot(out);
    if (via_[1]) via_[1]->appendSnapshot(out);
    if (ay_[0])  ay_[0]->appendSnapshot(out);
    if (ay_[1])  ay_[1]->appendSnapshot(out);
    if (ssi_)    ssi_->appendSnapshot(out);
}

void MockingboardCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    std::lock_guard<std::mutex> lk(mtx);
    pom2::byteio::Reader r(data, len);
    if (!r.has(6)) return;
    if (r.u8() != 'M' || r.u8() != 'B' || r.u8() != 'S' || r.u8() != 1) return;
    (void)r.u8();                       // variant — informational
    const uint8_t present = r.u8();
    auto loadVia = [&](std::unique_ptr<pom2::Via6522>& v) -> bool {
        if (!r.has(pom2::Via6522::kSnapshotBytes)) return false;
        if (v) v->loadSnapshot(r.p + r.pos);
        r.pos += pom2::Via6522::kSnapshotBytes;
        return true;
    };
    auto loadAy = [&](std::unique_ptr<pom2::Ay3_8910>& a) -> bool {
        if (!r.has(pom2::Ay3_8910::kSnapshotBytes)) return false;
        if (a) a->loadSnapshot(r.p + r.pos);
        r.pos += pom2::Ay3_8910::kSnapshotBytes;
        return true;
    };
    auto loadSsi = [&]() -> bool {
        if (!r.has(pom2::Ssi263::kSnapshotBytes)) return false;
        if (ssi_) ssi_->loadSnapshot(r.p + r.pos);
        r.pos += pom2::Ssi263::kSnapshotBytes;
        return true;
    };
    if ((present & 0x01) && !loadVia(via_[0])) return;
    if ((present & 0x02) && !loadVia(via_[1])) return;
    if ((present & 0x04) && !loadAy(ay_[0]))   return;
    if ((present & 0x08) && !loadAy(ay_[1]))   return;
    if ((present & 0x10) && !loadSsi())        return;
}

void MockingboardCard::onReset()
{
    std::lock_guard<std::mutex> lk(mtx);
    via_[0]->reset();
    via_[1]->reset();
    ay_[0]->reset();
    ay_[1]->reset();
    if (ssi_) ssi_->reset();
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

bool MockingboardCard::snapshotSsi263(Ssi263Snap* out) const
{
    if (!ssi_ || !out) return false;
    std::lock_guard<std::mutex> lk(mtx);
    for (int r = 0; r <= pom2::Ssi263::REG_FILFREQ; ++r) {
        out->regs[r] = ssi_->peekRegister(static_cast<uint8_t>(r));
    }
    out->currentPhoneme         = ssi_->currentPhoneme();
    out->mode                   = static_cast<uint8_t>(ssi_->currentMode());
    out->aRequest               = ssi_->aRequest();
    out->powerDown              = ssi_->powerDown();
    out->irqEnabled             = ssi_->irqEnabled();
    out->phonemeRemainingCycles = ssi_->phonemeRemainingCycles();
    out->phonemeWriteCount      = ssi_->phonemeWriteCount();
    return true;
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
    syncToCpuCycleAt(cpu_->getCycleCountNow());
}

// Advance the VIAs to an explicit absolute CPU cycle. Split out from
// syncToCpuCycle() so the end-of-step batch path can pass the CORRECTED
// "now": at that point Memory::advanceCycles has already folded the slice
// into cycleCounter while cpu->cycles still holds it, so getCycleCountNow()
// overshoots by one instruction (see advanceCycles).
void MockingboardCard::syncToCpuCycleAt(uint64_t now)
{
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
    // Address decode:
    //   Variant::AC      — bit 7 selects VIA, bits 0..3 select register,
    //                      bits 4..6 are partial-decode mirrors.
    //   Variant::SoundII — same, EXCEPT $40-$4F is the SSI263 (5 regs +
    //                      mirrors) overriding what would otherwise be
    //                      a VIA1 mirror at those addresses.
    std::lock_guard<std::mutex> lk(mtx);
    syncToCpuCycle();     // make T1/T2/IFR cycle-accurate at "now"
    if (ssi_ && (low8 & 0xF8) == 0x40) {
        return ssi_->read(low8 & 0x07);
    }
    const int chip = (low8 & 0x80) ? 1 : 0;
    const uint8_t out = via_[chip]->read(low8 & 0x0F);
    updateIrq();          // T1CL clears IFR.T1, may drop IRQ
    return out;
}

void MockingboardCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx);
    syncToCpuCycle();     // T1 counters reflect "now" before T1CH reload
    if (ssi_ && (low8 & 0xF8) == 0x40) {
        // SSI263 register write. The chip's own write() acks A/!R
        // internally for $00..$02 — the host CPU clears the VIA's
        // IFR.CA1 separately (typical Mockingboard Sound II driver
        // writes the CA1 bit to IFR after each phoneme).
        ssi_->write(low8 & 0x07, v);
        updateIrq();
        return;
    }
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
    // Tick the SSI263 (Sound II only) and surface A/!R end-of-phoneme
    // to VIA1.CA1 — real card wires SSI263.A/!R inverted into CA1, so
    // a 0→1 of A/!R = negative edge on CA1, matching PCR.0 == 0 (the
    // default config used by stock Sound II drivers).
    if (ssi_ && ssi_->advance(cycles)) {
        via_[0]->setCa1NegativeEdge();
    }
    if (cpu_) {
        // Lazy-sync path: any cycles already accounted for via MMIO accesses
        // during this slice were advanced by syncToCpuCycle() already; catch
        // up the remainder so end-of-slice IRQ state is published.
        //
        // Memory::advanceCycles ran `cycleCounter += cycles` BEFORE dispatching
        // to us, yet M6502::step() hasn't cleared cpu->cycles (still == this
        // `cycles`). So getCycleCountNow() == cycleCounter + cpu->cycles
        // overshoots the true "now" by exactly `cycles`. Subtract them, or the
        // VIAs jump one instruction ahead and the next mid-instruction MMIO
        // read hits the `now <= lastSyncCycle_` early-out — losing cycle
        // accuracy precisely where the lazy-sync was meant to provide it.
        syncToCpuCycleAt(cpu_->getCycleCountNow() - static_cast<uint64_t>(cycles));
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

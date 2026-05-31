// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PhasorCard — see PhasorCard.h for the architecture + soft-switch rules.

#include "PhasorCard.h"

#include "ByteIO.h"
#include "CpuClock.h"
#include "M6502.h"

#include <algorithm>
#include <cstring>

namespace {

// AY-3-8913 input clock — same wiring as Mockingboard's AY-3-8910 (pin
// 22 = slot phase 0 = 1.0227 MHz). Phasor doubles the chip clock in
// native mode (`PhasorCard::clockScale() == 2`); the synth multiplies
// the per-sample step by `clockScale` so registers produce notes one
// octave higher in Phasor mode than the same values would on a
// Mockingboard.
constexpr float kAyClockHz       = static_cast<float>(POM2_CPU_CLOCK_HZ);
constexpr float kAyToneStepHz    = kAyClockHz / 8.0f;
constexpr float kAyNoiseStepHz   = kAyClockHz / 8.0f;

// AY-3-8910 / 8913 logarithmic 4-bit volume → linear amplitude.
// Sourced from MAME `ay8910.cpp` `build_single_table(normalize=1)` with
// `ay8910_param`. Index 0 = silence, 15 = peak.
constexpr float kAyVolumeTable[16] = {
    0.0000f, 0.0105f, 0.0154f, 0.0223f, 0.0321f, 0.0468f, 0.0635f, 0.1061f,
    0.1319f, 0.2164f, 0.2974f, 0.3909f, 0.5128f, 0.6371f, 0.8186f, 1.0000f
};

constexpr int kAyNumRegs = pom2::Ay3_8910::kAyNumRegs;

}  // namespace

// ─── AudioSrc ─────────────────────────────────────────────────────────────
//
// 4-AY mono mix, ported from `MockingboardCard::AudioSrc` (2-chip) and
// widened: 4 `ChipState` slots, 4 register-bank snapshots, summed mono
// output divided by 12 (peak = 4 chips × 3 channels × 1.0). The synth
// loop body is verbatim from MAME-parity Mockingboard — same tone
// counter (integer + fractional accumulator), same 17-bit LFSR noise,
// same 4-flag envelope state machine.
//
// What differs from Mockingboard's AudioSrc:
//
//   * 4 chips instead of 2 (and the matching wider snapshot arrays).
//   * Per-fill snapshot of `clockScale()` — Phasor-native mode (mode_
//     == PH_Phasor) doubles the AY input clock, so the per-sample
//     step rate for tone/noise/envelope counters is `clockScale × base
//     step`. Snapshotted under the parent mutex so a mid-fill mode
//     switch doesn't mid-tear the rate; the next fill picks up the new
//     scale.
//   * Mix divisor = 12 (vs MB's 6). MB-mode software which only writes
//     AY1 + AY3 (the primary AYs) will therefore sound ~6 dB quieter on
//     a Phasor in MB-compat mode than on a real Mockingboard at the
//     same volume knob position — the user can crank Phasor's slider
//     to compensate. The alternative (divide by 6) would clip when
//     a 4-AY Phasor-native driver hits full amplitude across all 4
//     chips. Predictable headroom wins.

struct PhasorCard::AudioSrc : public AudioSource, public RateAware
{
    explicit AudioSrc(PhasorCard* p) : parent(p) {}

    PhasorCard* parent;

    std::atomic<uint32_t> sampleRate { AudioDevice::kSampleRate };
    std::atomic<float>    volume     { 0.5f };
    std::atomic<bool>     muted      { false };

    void setSampleRate(uint32_t hz) override
    {
        if (hz == 0) hz = AudioDevice::kSampleRate;
        sampleRate.store(hz, std::memory_order_relaxed);
    }

    // Per-chip synthesis state (identical to MockingboardCard's
    // ChipState — counters, LFSR seed, envelope state machine, MAME
    // parity). Replicated here rather than extracted to a shared
    // header to keep the audio-thread state self-contained; an
    // `AyPsgSynth` shared base is a viable future refactor once the
    // two cards have settled.
    struct ChipState {
        uint16_t toneCounter[3] = { 0, 0, 0 };
        float    toneAccum  [3] = { 0, 0, 0 };
        uint8_t  toneOut    [3] = { 0, 0, 0 };
        uint16_t noiseCounter         = 0;
        float    noiseAccum           = 0;
        uint32_t noiseLfsr            = 1;     // MAME ay8910.cpp:1309
        uint8_t  noiseOut             = 0;
        uint8_t  noisePrescale        = 0;
        uint32_t lastSeenResetCount   = 0;
        uint32_t envCounter           = 0;
        float    envAccum             = 0;
        int      envStep              = 15;
        uint8_t  envAttack            = 0;
        uint8_t  envHold              = 0;
        uint8_t  envAlternate         = 0;
        uint8_t  envHolding           = 0;
        int      lastShape            = -1;
        uint32_t lastSeenEnvWriteCount = 0;
        bool     envRetrigger         = false;
    };
    ChipState chip[4];

    void fillAudioBuffer(float* output, int frameCount) override
    {
        if (frameCount <= 0) return;
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        if (sr == 0) { std::fill_n(output, frameCount, 0.0f); return; }
        const bool  isMuted = muted.load(std::memory_order_relaxed);
        const float vol     = volume.load(std::memory_order_relaxed);

        // Snapshot all 4 chips' register banks + reset/env-write counts
        // + the current clock scale under the parent mutex. ~88 bytes
        // of memcpy plus 9 ints — brief enough that CPU-thread VIA
        // contention is bounded.
        uint8_t  regSnap[4][kAyNumRegs];
        uint32_t resetCountSnap[4];
        uint32_t envWriteCountSnap[4];
        int      clockScaleSnap = 1;
        {
            std::lock_guard<std::mutex> lk(parent->mtx_);
            for (int ci = 0; ci < 4; ++ci) {
                std::memcpy(regSnap[ci],
                            parent->ay_[ci]->regs, kAyNumRegs);
                resetCountSnap[ci]    = parent->ayResetCount_[ci];
                envWriteCountSnap[ci] = parent->ayEnvWriteCount_[ci];
            }
            clockScaleSnap = parent->clockScale();
        }
        for (int ci = 0; ci < 4; ++ci) {
            if (chip[ci].lastSeenResetCount != resetCountSnap[ci]) {
                chip[ci].lastSeenResetCount = resetCountSnap[ci];
                chip[ci].noiseLfsr     = 1;
                chip[ci].noisePrescale = 0;
                chip[ci].noiseOut      = 0;
            }
            if (chip[ci].lastSeenEnvWriteCount != envWriteCountSnap[ci]) {
                chip[ci].lastSeenEnvWriteCount = envWriteCountSnap[ci];
                chip[ci].envRetrigger = true;
            }
        }

        if (isMuted) {
            std::fill_n(output, frameCount, 0.0f);
            return;
        }

        // Per-sample step rates. Phasor-native mode doubles the AY chip
        // clock (clockScale == 2), so every counter ticks twice as fast
        // per audio sample → registers produce notes one octave up.
        const float scale = static_cast<float>(clockScaleSnap);
        const float toneStepPerSample  = (kAyToneStepHz  * scale) / static_cast<float>(sr);
        const float noiseStepPerSample = (kAyNoiseStepHz * scale) / static_cast<float>(sr);

        for (int i = 0; i < frameCount; ++i) {
            float sample = 0.0f;
            for (int ci = 0; ci < 4; ++ci) {
                ChipState& cs = chip[ci];
                const uint8_t* r = regSnap[ci];

                const auto tonePeriod = [&](int ch) -> uint16_t {
                    const int lo = r[ch * 2];
                    const int hi = r[ch * 2 + 1] & 0x0F;
                    const int p  = (hi << 8) | lo;
                    return static_cast<uint16_t>(p == 0 ? 1 : p);
                };

                // ── Tone counters ──────────────────────────────────
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

                // ── Noise (17-bit LFSR x^17 + x^14 + 1) ────────────
                const uint16_t noisePer = static_cast<uint16_t>(
                    (r[6] & 0x1F) ? (r[6] & 0x1F) : 1);
                cs.noiseAccum += noiseStepPerSample;
                while (cs.noiseAccum >= 1.0f) {
                    cs.noiseAccum -= 1.0f;
                    if (++cs.noiseCounter < noisePer) continue;
                    cs.noiseCounter = 0;
                    cs.noisePrescale ^= 1;
                    if (cs.noisePrescale) continue;
                    const uint32_t bit =
                        ((cs.noiseLfsr >> 0) ^ (cs.noiseLfsr >> 3)) & 1;
                    cs.noiseLfsr = (cs.noiseLfsr >> 1) | (bit << 16);
                    cs.noiseOut  = static_cast<uint8_t>(cs.noiseLfsr & 1);
                }

                // ── Envelope (MAME 4-flag state machine) ───────────
                {
                    const int shape = r[13] & 0x0F;
                    if (shape != cs.lastShape || cs.envRetrigger) {
                        cs.envRetrigger = false;
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
                const uint8_t envOut = static_cast<uint8_t>(
                    cs.envStep ^ cs.envAttack);

                // ── Mixer (R7) + per-channel amplitude (R8/R9/R10) ─
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
            // 4 chips × 3 channels × peak 1.0 = 12.0 max amplitude.
            // Pre-divide so full-scale signal hits 1.0 before vol knob.
            output[i] = (sample / 12.0f) * vol;
        }
    }
};

// ─── PhasorCard ───────────────────────────────────────────────────────────

PhasorCard::PhasorCard(int slotNum)
    : slot_(slotNum)
{
    via_[0] = std::make_unique<pom2::Via6522>();
    via_[1] = std::make_unique<pom2::Via6522>();
    for (int i = 0; i < 4; ++i) ay_[i] = std::make_unique<pom2::Ay3_8910>();
    audio_  = std::make_unique<AudioSrc>(this);
    onReset();
}

PhasorCard::~PhasorCard() = default;

void PhasorCard::onUnplug()
{
    // SlotBus auto-releases pending IRQ on detach.
}

// ─── Rewind / snapshot ──────────────────────────────────────────────────────
// The 2 VIAs + 4 AYs register/timer state plus the Phasor mode soft-switch
// (it changes address decode + AY clock scale). Self-describing blob.
void PhasorCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    using namespace pom2::byteio;
    std::lock_guard<std::mutex> lk(mtx_);
    putU8(out, 'P'); putU8(out, 'H'); putU8(out, 'S'); putU8(out, 1);  // magic + ver
    putU8(out, static_cast<uint8_t>(mode_));
    uint8_t present = 0;
    if (via_[0]) present |= 0x01;
    if (via_[1]) present |= 0x02;
    for (int i = 0; i < 4; ++i) if (ay_[i]) present |= static_cast<uint8_t>(0x04 << i);
    putU8(out, present);
    if (via_[0]) via_[0]->appendSnapshot(out);
    if (via_[1]) via_[1]->appendSnapshot(out);
    for (int i = 0; i < 4; ++i) if (ay_[i]) ay_[i]->appendSnapshot(out);
}

void PhasorCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    pom2::byteio::Reader r(data, len);
    if (!r.has(6)) return;
    if (r.u8() != 'P' || r.u8() != 'H' || r.u8() != 'S' || r.u8() != 1) return;
    mode_ = static_cast<Mode>(r.u8());
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
    if ((present & 0x01) && !loadVia(via_[0])) return;
    if ((present & 0x02) && !loadVia(via_[1])) return;
    for (int i = 0; i < 4; ++i)
        if ((present & (0x04 << i)) && !loadAy(ay_[i])) return;
}

void PhasorCard::onReset()
{
    std::lock_guard<std::mutex> lk(mtx_);
    via_[0]->reset();
    via_[1]->reset();
    for (int i = 0; i < 4; ++i) ay_[i]->reset();
    mode_ = PH_Mockingboard;       // power-up default = MB compat
    assertIrq(false);
    lastSyncCycle_ = cpu_ ? cpu_->getCycleCountNow() : 0;
    viaWriteCount_[0] = viaWriteCount_[1] = 0;
    for (int i = 0; i < 4; ++i) {
        ayWriteCount_[i]    = 0;
        ayResetCount_[i]    = 0;
        ayEnvWriteCount_[i] = 0;
    }
}

AudioSource* PhasorCard::audioSource() { return audio_.get(); }

void PhasorCard::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = AudioDevice::kSampleRate;
    audio_->sampleRate.store(hz, std::memory_order_relaxed);
}

void PhasorCard::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    audio_->volume.store(v, std::memory_order_relaxed);
}

float PhasorCard::getVolume() const
{
    return audio_->volume.load(std::memory_order_relaxed);
}

void PhasorCard::setMuted(bool m)
{
    audio_->muted.store(m, std::memory_order_relaxed);
}

bool PhasorCard::isMuted() const
{
    return audio_->muted.load(std::memory_order_relaxed);
}

// ─── VIA lazy-sync (same pattern as MockingboardCard) ────────────────────

void PhasorCard::syncToCpuCycle()
{
    if (!cpu_) return;
    syncToCpuCycleAt(cpu_->getCycleCountNow());
}

void PhasorCard::syncToCpuCycleAt(uint64_t now)
{
    if (now <= lastSyncCycle_) {
        // Defensive rewind (mirrors MockingboardCard::syncToCpuCycleAt):
        // the end-of-step batch path passes (getCycleCountNow() - cycles),
        // which can be < lastSyncCycle_ if a mid-instruction MMIO access
        // already synced past that point. Pin lastSyncCycle_ to the
        // smaller value so the next syncs the freshly-elapsed delta and
        // doesn't no-op every batch tick.
        lastSyncCycle_ = now;
        return;
    }
    const int delta = static_cast<int>(now - lastSyncCycle_);
    via_[0]->advance(delta);
    via_[1]->advance(delta);
    lastSyncCycle_ = now;
    updateIrq();
}

// ─── Slot ROM ($Cs00-$CsFF) — dual VIA decode ────────────────────────────

uint8_t PhasorCard::slotRomRead(uint8_t low8)
{
    std::lock_guard<std::mutex> lk(mtx_);
    syncToCpuCycle();
    // VIA1 at $00-$7F (mirrors of $00-$0F), VIA2 at $80-$FF.
    const int chip = (low8 & 0x80) ? 1 : 0;
    const uint8_t out = via_[chip]->read(low8 & 0x0F);
    updateIrq();
    return out;
}

void PhasorCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx_);
    syncToCpuCycle();
    const int chip = (low8 & 0x80) ? 1 : 0;
    const uint8_t events = via_[chip]->write(low8 & 0x0F, v);
    ++viaWriteCount_[chip];
    // Port A / Port B output change triggers AY routing.
    if (events & 0x03) onViaPortBChange(chip);
    updateIrq();
}

// ─── Device select ($C0(8+s)X) — mode soft-switch ────────────────────────

uint8_t PhasorCard::deviceSelectRead(uint8_t low4)
{
    // Real Phasor mode-switch responds to BOTH reads and writes, with
    // identical bit decoding (the address — not the data — drives the
    // mode). Status read returns the current mode for inspection.
    applyModeSwitch(low4);
    return static_cast<uint8_t>(mode_);
}

void PhasorCard::deviceSelectWrite(uint8_t low4, uint8_t /*v*/)
{
    applyModeSwitch(low4);
}

void PhasorCard::applyModeSwitch(uint8_t offset)
{
    std::lock_guard<std::mutex> lk(mtx_);
    uint8_t m = static_cast<uint8_t>(mode_);
    if (offset & 0x8) m &= ~0x7;      // bit 3 clears mode bits 2:0
    m |= (offset & 0x7);              // OR in low 3 bits
    mode_ = static_cast<Mode>(m);
}

// ─── AY routing ──────────────────────────────────────────────────────────

void PhasorCard::onViaPortBChange(int viaIdx)
{
    // The two AY chips controlled by this VIA. VIA0 → {AY0, AY1};
    // VIA1 → {AY2, AY3}. Index 0 = primary, 1 = secondary.
    const int ayBase = (viaIdx == 0) ? 0 : 2;

    auto& v  = *via_[viaIdx];
    const uint8_t pa = v.portAOut & v.ddrA;
    const uint8_t pb = v.portBOut & v.ddrB;

    // /RESET (PB2 low) is handled BEFORE the chip-select decode. MAME's
    // a2bus_phasor via_out_b resets the AY pair outside the chip_sel
    // computation: `m_ay1->reset_w(); if (m_native) m_ay2->reset_w();` — so
    // a reset strobe is never gated by PB3/PB4. In Mockingboard-compat mode
    // only the primary AY resets; in Phasor-native mode the whole pair does.
    // The old code routed reset through the chip-select mask, leaving the
    // secondary chip with stale register/LFSR/envelope state whenever native
    // software pulses /RESET without re-asserting the per-chip select bits.
    if ((pb & pom2::Ay3_8910::kPbBitReset) == 0) {
        ay_[ayBase]->reset();
        ++ayResetCount_[ayBase];
        if (mode_ != PH_Mockingboard) {
            ay_[ayBase + 1]->reset();
            ++ayResetCount_[ayBase + 1];
        }
        return;
    }

    // Decide which of the two AYs in the pair receive this strobe.
    // bit0 = primary, bit1 = secondary.
    int targetMask = 0;
    if (mode_ == PH_Mockingboard) {
        // Compat: primary AY only (the secondary "extra" Phasor chip
        // stays silent on MB-mode software).
        targetMask = 0b01;
    } else {
        // Phasor native (and EchoPlus, treated identically for routing):
        // chip_sel = ~(pb >> 3) & 3 — active-low select on PB3..PB4.
        //   0 → no chip   |  1 → primary
        //   2 → secondary |  3 → BOTH (broadcast)
        const uint8_t chipSel = static_cast<uint8_t>((~pb >> 3) & 0x3);
        targetMask = chipSel;
    }

    auto routeOne = [&](int chipIdx) {
        const auto res = ay_[chipIdx]->applyControl(pa, pb);
        if (res == pom2::Ay3_8910::ApplyResult::Wrote) {
            ++ayWriteCount_[chipIdx];
            // R13 (envelope shape) — bump the env-write counter so the
            // audio thread restarts the envelope on the next fill, even
            // when the shape value is unchanged (real AY behaviour:
            // set_shape runs on every R13 store).
            if ((ay_[chipIdx]->latchedAddr & 0x0F) == 13) {
                ++ayEnvWriteCount_[chipIdx];
            }
        } else if (res == pom2::Ay3_8910::ApplyResult::ResetOnly) {
            ++ayResetCount_[chipIdx];
        }
    };
    if (targetMask & 0b01) routeOne(ayBase);
    if (targetMask & 0b10) routeOne(ayBase + 1);
}

// ─── Cycle pacing + IRQ ──────────────────────────────────────────────────

void PhasorCard::advanceCycles(int cycles)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (cpu_) {
        // Same protocol as MockingboardCard::advanceCycles — sync up to
        // (now - cycles) only. Memory::advanceCycles folded `cycles` into
        // cycleCounter BEFORE dispatching, yet cpu->cycles still holds
        // them, so getCycleCountNow() overshoots the true "now" by
        // exactly one instruction. Subtracting `cycles` lands us at the
        // real end-of-instruction time; the VIAs are then correctly at
        // that point. Adding another via_->advance(cycles) here would
        // double-charge T1 by one instruction per slice (pinned by
        // testNoEndOfStepOvershoot in phasor_card_smoke).
        syncToCpuCycleAt(cpu_->getCycleCountNow() -
                         static_cast<uint64_t>(cycles));
    } else {
        via_[0]->advance(cycles);
        via_[1]->advance(cycles);
    }
    updateIrq();
}

void PhasorCard::updateIrq()
{
    const bool combined = via_[0]->irqOut() || via_[1]->irqOut();
    assertIrq(combined);
}

uint8_t PhasorCard::getAyRegister(int chip, int reg) const
{
    if (chip < 0 || chip > 3 || reg < 0 || reg > 15) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    return ay_[chip]->regs[reg];
}

uint8_t PhasorCard::peekViaRegister(int chip, int reg) const
{
    if (chip < 0 || chip > 1 || reg < 0 || reg > 15) return 0xFF;
    std::lock_guard<std::mutex> lk(mtx_);
    auto& v = *via_[chip];
    switch (reg & 0x0F) {
    case pom2::Via6522::VIA_ORB:    return v.readPortB();
    case pom2::Via6522::VIA_ORA:    return v.readPortA();
    case pom2::Via6522::VIA_DDRB:   return v.ddrB;
    case pom2::Via6522::VIA_DDRA:   return v.ddrA;
    case pom2::Via6522::VIA_T1CL:   return static_cast<uint8_t>(v.t1Counter & 0xFF);
    case pom2::Via6522::VIA_T1CH:   return static_cast<uint8_t>((v.t1Counter >> 8) & 0xFF);
    case pom2::Via6522::VIA_T1LL:   return static_cast<uint8_t>(v.t1Latch & 0xFF);
    case pom2::Via6522::VIA_T1LH:   return static_cast<uint8_t>((v.t1Latch >> 8) & 0xFF);
    case pom2::Via6522::VIA_T2CL:   return static_cast<uint8_t>(v.t2Counter & 0xFF);
    case pom2::Via6522::VIA_T2CH:   return static_cast<uint8_t>((v.t2Counter >> 8) & 0xFF);
    case pom2::Via6522::VIA_SR:     return v.sr;
    case pom2::Via6522::VIA_ACR:    return v.acr;
    case pom2::Via6522::VIA_PCR:    return v.pcr;
    case pom2::Via6522::VIA_IFR:    return v.computedIfr();
    case pom2::Via6522::VIA_IER:    return static_cast<uint8_t>(v.ier | 0x80);
    case pom2::Via6522::VIA_ORANH:  return v.readPortA();
    default:                        return 0xFF;
    }
}

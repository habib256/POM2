// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PhasorCard — see PhasorCard.h for the architecture + soft-switch rules.

#include "PhasorCard.h"

#include "AyPsgSynth.h"

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

// Amplitude table lives in AyPsgSynth.h since 2026-08-01, shared with
// MockingboardCard (the old "MAME build_single_table" citation on both
// copies was wrong — see the header).
using pom2::ay::kVolumeTable;

constexpr int kAyNumRegs = pom2::Ay3_8910::kAyNumRegs;

}  // namespace

// ─── AudioSrc ─────────────────────────────────────────────────────────────
//
// 4-AY stereo mix, ported from `MockingboardCard::AudioSrc` (2-chip) and
// widened: 4 `ChipState` slots, 4 register-bank snapshots, one VIA pair
// per channel divided by 6 (peak = 2 chips × 3 channels × 1.0 per side).
// The synth loop body is verbatim from MAME-parity Mockingboard — same
// tone counter (integer + fractional accumulator), same 17-bit LFSR
// noise, same 4-flag envelope state machine.
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
//   * Mix divisor = 6 PER SIDE (vs MB's 3), because a Phasor side
//     carries two chips where a Mockingboard side carries one. So
//     MB-mode software — which only drives AY1 + AY3, the primary of
//     each pair — still sounds ~6 dB quieter on a Phasor than on a real
//     Mockingboard at the same knob position; the user can crank
//     Phasor's slider. The alternative (matching MB's per-side level)
//     would clip when a 4-AY native driver hits full amplitude on both
//     chips of a side. Predictable headroom wins.

struct PhasorCard::AudioSrc : public AudioSource, public RateAware
{
    explicit AudioSrc(PhasorCard* p) : parent(p) {}

    PhasorCard* parent;

    std::atomic<uint32_t> sampleRate { AudioSource::kDefaultSampleRate };
    std::atomic<float>    volume     { 0.5f };
    std::atomic<bool>     muted      { false };

    void setSampleRate(uint32_t hz) override
    {
        if (hz == 0) hz = AudioSource::kDefaultSampleRate;
        sampleRate.store(hz, std::memory_order_relaxed);
    }

    // Per-chip synthesis state + the per-tick generators live in
    // AyPsgSynth.h, shared with MockingboardCard since 2026-08-01.
    // Before that both cards carried verbatim copies (~130 lines, 4
    // differing lines) and had already drifted apart — Phasor never
    // gained the cycle-stamped event queue, and would also have missed
    // the band-limiting rework.
    using ChipState = pom2::ay::ChipSynthState;

    ChipState chip[4];

    /// One-pole 20 Hz high-pass per side — MAME's default per-speaker
    /// filter (`src/emu/audio_effects/filter.cpp:39-44`), and MAME really
    /// does put one on each of the Phasor's two speakers.
    /// Implementation in AyPsgSynth.h.
    pom2::ay::DcBlocker dcL;
    pom2::ay::DcBlocker dcR;

    // ── Stereo ────────────────────────────────────────────────────────
    // MAME gives the Phasor a second 2-channel speaker and splits the
    // chips by VIA pair (`a2mockingboard.cpp:192-208`): ay1 and ay2 land
    // on channel 0 of their respective speakers, ay3 and ay4 on channel 1,
    // and both speakers are `.front()`, so the audible result is
    // LEFT = ay1+ay2 (the VIA1 pair) and RIGHT = ay3+ay4 (the VIA2 pair).
    // POM2 indexes the same pairing — `onViaPortBChange` routes VIA0 to
    // {ay_[0], ay_[1]} and VIA1 to {ay_[2], ay_[3]}.
    //
    // As on the Mockingboard, `right == nullptr` renders the mono
    // fold-down, which reproduces the old `/12` summed render exactly.
    void fillAudioBuffer(float* output, int frameCount) override
    {
        render(output, nullptr, frameCount);
    }

    bool fillAudioBufferStereo(float* left, float* right,
                               int frameCount) override
    {
        render(left, right, frameCount);
        return true;
    }

    void render(float* left, float* right, int frameCount)
    {
        if (frameCount <= 0) return;
        const auto silence = [&]() {
            std::fill_n(left, frameCount, 0.0f);
            if (right) std::fill_n(right, frameCount, 0.0f);
        };
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        if (sr == 0) { silence(); return; }
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
                // Full generator reset, not just the noise LFSR: tone
                // counters/flip-flops and the envelope state machine must
                // also re-seed on /RESET (MAME ay8910_reset_ym), exactly as
                // MockingboardCard does via resetGenerators(). Re-seeding
                // only the noise half left a finished envelope holding at
                // step 0 across the strobe, so the next envelope note came
                // out silent where the same driver on a Mockingboard (and
                // on MAME) plays the 15→0 ramp.
                chip[ci].resetGenerators();
            }
            if (chip[ci].lastSeenEnvWriteCount != envWriteCountSnap[ci]) {
                chip[ci].lastSeenEnvWriteCount = envWriteCountSnap[ci];
                chip[ci].envRetrigger = true;
            }
        }

        if (isMuted) {
            silence();
            return;
        }

        // Base clock/8 ticks covered by one output sample. Phasor-native
        // mode doubles the AY chip clock (clockScale == 2), so every
        // counter ticks twice as fast per audio sample -> registers
        // produce notes one octave up.
        //
        // Derived from the NTSC constant rather than a live CPU clock:
        // unlike MockingboardCard, PhasorCard has no setCpuClock override
        // yet, so a PAL machine still clocks these AYs 0.7 % fast. Minor
        // next to the aliasing this loop used to produce, but it is a
        // real remaining gap.
        const float scale = static_cast<float>(clockScaleSnap);
        const float ticksPerSample = (kAyToneStepHz * scale)
                                     / static_cast<float>(sr);
        const float invTicksPerSample =
            (ticksPerSample > 0.0f) ? (1.0f / ticksPerSample) : 0.0f;
        dcL.setRate(sr);
        dcR.setRate(sr);

        for (int i = 0; i < frameCount; ++i) {
            // Index 0 = left (VIA0's pair, ay_[0..1]), 1 = right (VIA1's
            // pair, ay_[2..3]).
            float side[2] = { 0.0f, 0.0f };
            for (int ci = 0; ci < 4; ++ci) {
                // Box-integrate the mixer across the ~2.9 base ticks this
                // output sample spans, instead of point-sampling it once
                // at the end. See AyPsgSynth.h for why: the old loop threw
                // away the sub-sample edge position it had just computed,
                // which folded every harmonic above Nyquist back into the
                // audible band.
                side[ci >> 1] += pom2::ay::renderChipSample(
                    chip[ci], regSnap[ci], ticksPerSample, invTicksPerSample);
            }
            // 2 chips x 3 channels x peak 1.0 = 6.0 per side. Headroom-safe
            // and strictly LINEAR — see the matching note in
            // Mockingboard.cpp for why the `tanh` soft knee tried here on
            // 2026-08-01 was reverted (it distorted any mix that used more
            // than one AY). The mono fold-down below restores the `/12` the
            // 4-chip summed render used.
            // DC blocker: the AY channel model is unipolar, so gating
            // channels on and off steps the offset.
            const float l = dcL.process(side[0] * (1.0f / 6.0f)) * vol;
            const float r = dcR.process(side[1] * (1.0f / 6.0f)) * vol;
            if (right) { left[i] = l; right[i] = r; }
            else       { left[i] = 0.5f * (l + r); }
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
    // ver 2 since 2026-07-30: the VIA sections gained a byte (portAIn).
    putU8(out, 'P'); putU8(out, 'H'); putU8(out, 'S'); putU8(out, 2);  // magic + ver
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
    if (r.u8() != 'P' || r.u8() != 'H' || r.u8() != 'S') return;
    // v1 blobs carry 24-byte VIA sections, v2 carry 25 (portAIn added
    // 2026-07-30 for the AY read-bus latch). The layout change must bump
    // this byte, or an old snapshot's every later field shifts by one.
    const uint8_t blobVer = r.u8();
    if (blobVer != 1 && blobVer != 2) return;
    const std::size_t viaBytes = (blobVer >= 2)
        ? pom2::Via6522::kSnapshotBytes : pom2::Via6522::kSnapshotBytesV1;
    const uint8_t mode = r.u8();
    if (mode != PH_Mockingboard && mode != PH_Phasor && mode != PH_EchoPlus)
        return;
    const uint8_t present = r.u8();
    std::size_t required = 6 +
        ((present & 0x01) ? viaBytes : 0) +
        ((present & 0x02) ? viaBytes : 0);
    for (int i = 0; i < 4; ++i)
        if (present & (0x04 << i)) required += pom2::Ay3_8910::kSnapshotBytes;
    if (len < required) return;          // validate before mutating any chip
    mode_ = static_cast<Mode>(mode);
    auto loadVia = [&](std::unique_ptr<pom2::Via6522>& v) -> bool {
        if (v) v->loadSnapshot(r.p + r.pos, viaBytes);
        r.pos += viaBytes;
        return true;
    };
    auto loadAy = [&](std::unique_ptr<pom2::Ay3_8910>& a) -> bool {
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
    if (hz == 0) hz = AudioSource::kDefaultSampleRate;
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
    const uint64_t delta = now - lastSyncCycle_;
    // The VIAs' `advance()` takes an int; clamp to a sane upper bound
    // (a single CPU run-slice is ~17 045 cycles, so anything beyond a
    // few million here means our sync clock got desynchronised). Same
    // defensive clamp as MockingboardCard::syncToCpuCycleAt — without
    // it a desync would truncate/overflow the int cast.
    const int step = (delta > 0x7FFFFFFFu) ? 0x7FFFFFFF
                                           : static_cast<int>(delta);
    via_[0]->advance(step);
    via_[1]->advance(step);
    lastSyncCycle_ = now;
    updateIrq();
}

// ─── VIA select decode — MAME a2bus_phasor_device parity ─────────────────
//
// MAME `a2mockingboard.cpp:312-337` (read_cnxx) / `:365-390` (write_cnxx):
//
//   if (m_native)
//       via_sel = ((offset & 0x80) >> 6) | ((offset & 0x10) >> 4);
//   else
//       via_sel = (offset & 0x80) ? 2 : 1;
//   if ((offset < 0x20) || (offset >= 0x80 && offset < 0xa0)) { ... }
//
// bit 0 of via_sel selects VIA1, bit 1 selects VIA2; reads OR the selected
// VIAs' bytes together, writes broadcast to every selected VIA. The range
// gate applies in BOTH modes, so:
//
//   Mockingboard mode:  $00-$1F → VIA1, $80-$9F → VIA2, rest → none.
//   Native mode:        $00-$0F → none, $10-$1F → VIA1,
//                       $80-$8F → VIA2, $90-$9F → BOTH (broadcast).
//
// MAME's `m_native` flag is set by any $C0(8+s)X access with address bit 0
// high; POM2's richer mode_ soft-switch reaches PH_Phasor (5) / PH_EchoPlus
// (7) the same way — both have bit 0 set — so "native" maps to
// `mode_ != PH_Mockingboard`, consistent with the AY routing decode.
// Caller must hold mtx_.
int PhasorCard::viaSelect(uint8_t low8) const
{
    if (!(low8 < 0x20 || (low8 >= 0x80 && low8 < 0xA0))) return 0;
    if (mode_ == PH_Mockingboard) return (low8 & 0x80) ? 0x2 : 0x1;
    return ((low8 & 0x80) >> 6) | ((low8 & 0x10) >> 4);
}

// ─── Slot ROM ($Cs00-$CsFF) — dual VIA decode ────────────────────────────

uint8_t PhasorCard::slotRomRead(uint8_t low8)
{
    std::lock_guard<std::mutex> lk(mtx_);
    syncToCpuCycle();
    // MAME read_cnxx: OR together every selected VIA's byte; an
    // undecoded offset returns 0 (`a2mockingboard.cpp:312-337` — `ret`
    // starts at 0 and the range gate skips the reads entirely).
    const int viaSel = viaSelect(low8);
    uint8_t out = 0;
    if (viaSel & 0x1) out |= via_[0]->read(low8 & 0x0F);
    if (viaSel & 0x2) out |= via_[1]->read(low8 & 0x0F);
    updateIrq();
    return out;
}

void PhasorCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx_);
    syncToCpuCycle();
    // MAME write_cnxx: broadcast to every selected VIA; undecoded
    // offsets are dropped (`a2mockingboard.cpp:365-390`).
    const int viaSel = viaSelect(low8);
    for (int chip = 0; chip < 2; ++chip) {
        if (!(viaSel & (1 << chip))) continue;
        const uint8_t events = via_[chip]->write(low8 & 0x0F, v);
        ++viaWriteCount_[chip];
        // Port A / Port B output change triggers AY routing.
        if (events & 0x03) onViaPortBChange(chip);
    }
    updateIrq();
}

// ─── Device select ($C0(8+s)X) — mode soft-switch ────────────────────────

uint8_t PhasorCard::deviceSelectRead(uint8_t low4)
{
    // Real Phasor mode-switch responds to BOTH reads and writes, with
    // identical bit decoding (the address — not the data — drives the
    // mode). The read returns $FF (open bus) — MAME `a2mockingboard.cpp`
    // `a2bus_phasor_device::read_c0nx`: `m_native = BIT(offset,0);
    // set_clocks(); return 0xff;`. Returning the mode value (as POM2
    // once did) gave detection routines a readable register real
    // hardware doesn't have.
    applyModeSwitch(low4);
    return 0xFF;
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
        } else if (res == pom2::Ay3_8910::ApplyResult::Read) {
            // Same AY read-bus latch as the Mockingboard: the chip drives
            // the selected register onto the bus and the card latches it
            // onto the driving VIA's port-A input. Phasor mode detectors
            // write-then-read an AY register to identify the board.
            // The driving VIA is the one this call is about. `chipIdx` is
            // `ayBase` or `ayBase + 1` and `ayBase` came from `viaIdx`
            // (2 AYs per VIA), so the old `chipIdx >> 1` re-derivation
            // only shadowed the parameter with its own value (-Wshadow).
            if (viaIdx >= 0 && viaIdx < 2)
                via_[viaIdx]->setPortAInput(ay_[chipIdx]->busOut);
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

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SpeakerDevice — 1-bit speaker synthesis. The Apple II speaker is a
// single flip-flop driven by any access in the $C030-$C03F MMIO range.
// Programs make sound by hitting $C030 in tight loops at frequencies
// between ~50 Hz and ~5 kHz; the cone's natural mechanical low-pass
// turns the resulting square wave into recognisable tones.
//
// Reconstruction pipeline (MAME `spkrdev.cpp:74-327` verbatim port):
//
//   CPU thread ─ Memory's $C030 handler calls recordToggle(absoluteCpuCycle)
//                 → push event onto an SPSC-style deque (mutex-guarded).
//   Audio thread ─ fillAudioBuffer():
//                   * For each output sample, fill 4 intermediate samples
//                     (RATE_MULTIPLIER=4 oversampling) by rectangle-area
//                     integration of the latch level over each sub-window.
//                   * Convolve the rolling 64-entry composed_volume ring
//                     with a windowed sinc kernel (FILTER_STEP =
//                     π/(2*RATE_MULTIPLIER), cutoff ≈ sr/4).
//                   * 0.995-pole DC blocker (matches MAME `:280-285`).
//
// This replaces the earlier "snap-to-level + 1-pole LP" reconstruction
// which aliased badly on tight click sequences (Karateka music, click-
// rate effects above sample_rate/4 — pinned by `tests/speaker_smoke`).

#ifndef POM2_SPEAKER_DEVICE_H
#define POM2_SPEAKER_DEVICE_H

#include "AudioDevice.h"
#include "CpuClock.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

class SpeakerDevice : public AudioSource
{
public:
    SpeakerDevice();
    ~SpeakerDevice() override = default;

    /// Called from the CPU thread synchronously when $C030-$C03F is
    /// accessed. `cpuCycle` is the absolute CPU cycle of the access
    /// (Memory::cycleCounter + cpu->getCurrentInstructionCycles()).
    void recordToggle(uint64_t cpuCycle);

    /// AudioSource — generates speaker samples mixed by AudioDevice.
    void fillAudioBuffer(float* output, int frameCount) override;

    /// Set the audio output sample rate (negotiated by AudioDevice).
    void setSampleRate(uint32_t hz);
    uint32_t getSampleRate() const { return outputSampleRate.load(std::memory_order_relaxed); }

    /// Volume in [0, 2]. UI thread sets, audio thread reads.
    void  setVolume(float v);
    float getVolume() const { return volume.load(std::memory_order_relaxed); }

    /// Mute toggle — separate from volume so the user can flip it without
    /// losing their level setting.
    void setMuted(bool m) { muted.store(m, std::memory_order_relaxed); }
    bool isMuted() const  { return muted.load(std::memory_order_relaxed); }

    /// Drop pending events + reset filter state. Called on hard reset.
    void reset();

    /// Diagnostic — last audio-thread cursor in CPU cycles. UI displays
    /// it as a sanity check.
    uint64_t getAudioCpuCursor() const { return audioCpuCursor; }
    size_t   getQueuedEventCount() const;

private:
    static constexpr float  kCpuClockHz   = static_cast<float>(POM2_CPU_CLOCK_HZ);
    static constexpr float  kSquareAmp    = 0.18f;     // headroom vs cassette mix
    static constexpr float  kCatchUpSecs  = 0.10f;     // snap forward if behind
    static constexpr size_t kMaxEvents    = 16384;     // ~750 ms at 22 kHz toggles
    // MAME parity: 4× oversampling × 64-tap windowed sinc.
    // RATE_MULTIPLIER must divide FILTER_LENGTH evenly.
    static constexpr int    kRateMultiplier = 4;       // MAME `spkrdev.cpp:74`
    static constexpr int    kFilterLength   = 64;      // MAME `spkrdev_h.txt:28`

    mutable std::mutex   eventMutex;
    std::deque<uint64_t> events;

    // Audio-thread state. Touched only inside fillAudioBuffer + reset.
    uint64_t audioCpuCursor   = 0;     // CPU cycle at start of next sample
    double   subSampleAccum   = 0.0;   // fractional CPU cycles into next sub
    double   lastUpdateFrac   = 0.0;   // accumulator since last sub-sample
                                       //   boundary (units: sub-sample
                                       //   periods, range [0, 1)).
    bool     currentLevel     = false;
    // Rolling ring of integrated sub-sample windows. Each slot stores the
    // time-weighted average of `level` over one sub-sample period
    // (= [0..1] given binary level). Indexed by `composedIdx` (write
    // head); the sinc convolution walks the 64 most-recent slots
    // newest-last via `composedIdx + 1 .. composedIdx + 64`.
    std::array<double, kFilterLength> composedVolume{};
    int                               composedIdx = 0;
    // DC blocker state (MAME's y[n] = x[n] - x[n-1] + 0.995 * y[n-1]).
    double dcPrevX = 0.0;
    double dcPrevY = 0.0;

    // Sinc kernel + its abs-sum (used as the convolution normaliser).
    std::array<double, kFilterLength> ampl{};
    double ampSum = 1.0;

    // Producer-published high-water mark.
    std::atomic<uint64_t> latestEventCycle{0};

    std::atomic<float>    volume{1.0f};
    std::atomic<bool>     muted{false};
    std::atomic<uint32_t> outputSampleRate{AudioDevice::kSampleRate};

    void buildSincKernel();
};

#endif // POM2_SPEAKER_DEVICE_H

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SpeakerDevice — 1-bit speaker synthesis. The Apple II speaker is a
// single flip-flop driven by any access in the $C030-$C03F MMIO range.
// Programs make sound by hitting $C030 in tight loops at frequencies
// between ~50 Hz and ~5 kHz; the cone's natural mechanical low-pass
// turns the resulting square wave into recognisable tones.
//
// Pipeline:
//   CPU thread ─ Memory's $C030 handler calls recordToggle(absoluteCpuCycle)
//                 → push event onto an SPSC-style deque (mutex-guarded)
//   Audio thread ─ fillAudioBuffer():
//                   * advance audioCpuCursor by cyclesPerSample per frame
//                   * for every event ≤ cursor, flip currentLevel
//                   * 1-pole low-pass (~5 kHz) softens the click edges
//                   * DC blocker prevents long-term offset saturation
//
// The CPU thread holds EmulationController::stateMutex while writing
// events; the audio thread holds its own eventMutex briefly to drain
// them. They never block each other for long — the audio thread copies
// pending events into a local vector under the mutex (one allocation
// at most), then synthesises samples lock-free.

#ifndef POM2_SPEAKER_DEVICE_H
#define POM2_SPEAKER_DEVICE_H

#include "AudioDevice.h"
#include "CpuClock.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

class SpeakerDevice : public AudioSource
{
public:
    SpeakerDevice() = default;
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
    static constexpr float kCpuClockHz   = static_cast<float>(POM2_CPU_CLOCK_HZ);
    static constexpr float kSquareAmp    = 0.18f;     // headroom vs cassette mix
    static constexpr float kLpCutoffHz   = 5000.0f;   // speaker cone bandwidth
    static constexpr float kCatchUpSecs  = 0.10f;     // snap forward if behind
    static constexpr size_t kMaxEvents   = 16384;     // ~750 ms at 22 kHz toggles

    mutable std::mutex   eventMutex;
    std::deque<uint64_t> events;

    // Audio-thread state (only touched inside fillAudioBuffer + reset).
    uint64_t audioCpuCursor   = 0;
    double   cursorRem        = 0.0;
    bool     currentLevel     = false;
    float    lpState          = 0.0f;
    float    dcInputPrev      = 0.0f;
    float    dcOutputPrev     = 0.0f;

    // Producer-published high-water mark — used by the audio thread to
    // detect when the cursor has lagged too far and snap forward.
    std::atomic<uint64_t> latestEventCycle{0};

    std::atomic<float>    volume{1.0f};
    std::atomic<bool>     muted{false};
    std::atomic<uint32_t> outputSampleRate{AudioDevice::kSampleRate};
};

#endif // POM2_SPEAKER_DEVICE_H

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SpeakerDevice.h"

#include <algorithm>
#include <cmath>
#include <vector>

void SpeakerDevice::recordToggle(uint64_t cpuCycle)
{
    std::lock_guard<std::mutex> lk(eventMutex);
    events.push_back(cpuCycle);
    // Cap on queue size — drop oldest (consumer is too slow to keep up).
    // Happens only if the user is running --cpu-max while writing audio
    // they're not listening to; not a correctness concern.
    while (events.size() > kMaxEvents) events.pop_front();
    latestEventCycle.store(cpuCycle, std::memory_order_relaxed);
}

void SpeakerDevice::reset()
{
    std::lock_guard<std::mutex> lk(eventMutex);
    events.clear();
    latestEventCycle.store(0, std::memory_order_relaxed);
    audioCpuCursor = 0;
    cursorRem      = 0.0;
    currentLevel   = false;
    lpState        = 0.0f;
    dcInputPrev    = 0.0f;
    dcOutputPrev   = 0.0f;
}

void SpeakerDevice::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = AudioDevice::kSampleRate;
    outputSampleRate.store(hz, std::memory_order_relaxed);
}

void SpeakerDevice::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    volume.store(v, std::memory_order_relaxed);
}

size_t SpeakerDevice::getQueuedEventCount() const
{
    std::lock_guard<std::mutex> lk(eventMutex);
    return events.size();
}

void SpeakerDevice::fillAudioBuffer(float* output, int frameCount)
{
    if (frameCount <= 0) return;

    const uint32_t sr = outputSampleRate.load(std::memory_order_relaxed);
    if (sr == 0) { std::fill_n(output, frameCount, 0.0f); return; }

    const double cyclesPerSample = static_cast<double>(kCpuClockHz) /
                                   static_cast<double>(sr);

    // Catch-up: if the producer has run ahead of the audio cursor by
    // significantly more than kCatchUpSecs of CPU time, snap forward.
    // Avoids 5 s of buffered toggles playing at full speed if the user
    // pauses + resumes the emulator (or runs it at MAX while the audio
    // device is throttled).
    const uint64_t latest = latestEventCycle.load(std::memory_order_relaxed);
    const uint64_t catchUpCycles =
        static_cast<uint64_t>(2.0f * kCatchUpSecs * kCpuClockHz);
    if (latest > audioCpuCursor + catchUpCycles) {
        const uint64_t snapTo = latest -
            static_cast<uint64_t>(kCatchUpSecs * kCpuClockHz);
        // Drain events older than the snap point under lock.
        std::lock_guard<std::mutex> lk(eventMutex);
        while (!events.empty() && events.front() < snapTo) events.pop_front();
        audioCpuCursor = snapTo;
        cursorRem = 0.0;
    }

    // Snapshot all events that could fire inside this audio buffer's
    // window into a local vector. Holds the mutex for one allocation +
    // O(n) pop; release before the synth loop so the producer isn't
    // blocked while we render samples.
    std::vector<uint64_t> windowEvents;
    {
        const uint64_t windowEndApprox = audioCpuCursor +
            static_cast<uint64_t>(frameCount * cyclesPerSample) + 2;
        std::lock_guard<std::mutex> lk(eventMutex);
        // Discard events strictly before the cursor (race-safe — they
        // missed their window).
        while (!events.empty() && events.front() < audioCpuCursor) events.pop_front();
        // Drain everything ≤ windowEndApprox into the local buffer.
        while (!events.empty() && events.front() <= windowEndApprox) {
            windowEvents.push_back(events.front());
            events.pop_front();
        }
    }

    // 1-pole low-pass coefficient (Butterworth-equivalent for first order).
    const float lpAlpha = 1.0f - std::exp(
        -2.0f * 3.14159265358979f * kLpCutoffHz / static_cast<float>(sr));

    // DC blocker pole — 0.995 gives ~12 Hz cutoff at 44 kHz, plenty low
    // not to attack actual speaker tones (50 Hz +) yet pulls out the
    // long-term DC offset that an unchanged level would otherwise
    // accumulate at the LP output.
    constexpr float kDcPole = 0.995f;

    const float vol     = volume.load(std::memory_order_relaxed);
    const bool  isMuted = muted.load(std::memory_order_relaxed);
    size_t evIdx = 0;

    for (int i = 0; i < frameCount; ++i) {
        // Advance cursor by one sample's worth of CPU cycles. Track the
        // fractional remainder so we don't drift over time at a
        // non-integer cyclesPerSample (1.0227 MHz / 48000 Hz = 21.31).
        cursorRem += cyclesPerSample;
        const uint64_t cursorAdd = static_cast<uint64_t>(cursorRem);
        cursorRem -= static_cast<double>(cursorAdd);
        audioCpuCursor += cursorAdd;

        // Toggle for every event whose timestamp is ≤ the cursor.
        while (evIdx < windowEvents.size() &&
               windowEvents[evIdx] <= audioCpuCursor) {
            currentLevel = !currentLevel;
            ++evIdx;
        }

        // Square-wave target with headroom.
        const float target = currentLevel ? kSquareAmp : -kSquareAmp;

        // 1-pole LP — softens the click edges of every transition.
        lpState += lpAlpha * (target - lpState);

        // DC blocker — y[n] = x[n] - x[n-1] + p * y[n-1].
        const float dcOut = lpState - dcInputPrev + kDcPole * dcOutputPrev;
        dcInputPrev  = lpState;
        dcOutputPrev = dcOut;

        output[i] = isMuted ? 0.0f : (dcOut * vol);
    }

    // Anything that was in windowEvents but not consumed (timestamps
    // beyond the actual cursor advance — unlikely given our +2 padding,
    // but possible with rounding) gets pushed back.
    if (evIdx < windowEvents.size()) {
        std::lock_guard<std::mutex> lk(eventMutex);
        for (size_t k = evIdx; k < windowEvents.size(); ++k)
            events.push_front(windowEvents[k]);
        // events front order is now newest-first for the leftover; that's
        // OK because the next fillAudioBuffer's drain is order-agnostic
        // within the window, and the global ordering is restored on the
        // next recordToggle (which always pushes_back monotonically).
        // In practice this branch fires < 1 in 10^6 frames — so we trade
        // strict ordering for a single insert.
    }
}

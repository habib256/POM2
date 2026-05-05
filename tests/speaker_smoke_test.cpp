// Smoke test for SpeakerDevice 1-bit synthesis. Pins the high-level
// behaviour without trying to assert exact sample values (the LP +
// DC-block filters interact non-trivially):
//
//   1. No events  → output is silence (filter settles to 0 within
//                   the buffer length thanks to the DC blocker)
//   2. Toggles at ~1 kHz → buffer has both positive and negative samples
//                          (the square wave is being reconstructed)
//   3. reset() empties the queue and zeroes filter state
//   4. Mute squashes output regardless of incoming events

#include "SpeakerDevice.h"
#include "CpuClock.h"

#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kSampleRate = 44100;
constexpr int      kFrameCount = 1024;     // ~23 ms at 44.1 kHz

// Fill `dst` with audio samples; convenience for the cases below.
void runOneBuffer(SpeakerDevice& s, std::vector<float>& dst) {
    dst.assign(kFrameCount, 999.0f);   // sentinel
    s.fillAudioBuffer(dst.data(), kFrameCount);
}

// Drive a square-wave-style toggle pattern at `freqHz` for `seconds`,
// pushing each toggle as a CPU-cycle timestamp starting at `startCycle`.
void driveToggles(SpeakerDevice& s, double freqHz, double seconds,
                  uint64_t startCycle) {
    const double cyclesPerHalfPeriod = POM2_CPU_CLOCK_HZ / (2.0 * freqHz);
    const int totalEdges = static_cast<int>(2.0 * freqHz * seconds);
    for (int i = 0; i < totalEdges; ++i) {
        const uint64_t t = startCycle +
            static_cast<uint64_t>(i * cyclesPerHalfPeriod);
        s.recordToggle(t);
    }
}

} // namespace

int main()
{
    std::vector<float> buf;

    // ── Case 1: silence ──────────────────────────────────────────────────
    {
        SpeakerDevice spk;
        spk.setSampleRate(kSampleRate);
        runOneBuffer(spk, buf);
        // Filter starts at 0; without events, the LP target is the current
        // level which is 0 → output stays at 0. Looser bound than 0.0f to
        // tolerate filter ringing if anyone re-tunes the cutoffs.
        for (int i = 0; i < kFrameCount; ++i) {
            assert(std::fabs(buf[i]) < 0.05f);
        }
    }

    // ── Case 2: ~1 kHz toggles → audible square wave ────────────────────
    {
        SpeakerDevice spk;
        spk.setSampleRate(kSampleRate);
        // Pre-warm the filter by running one silent buffer at cycle 0.
        runOneBuffer(spk, buf);
        // Drive ~50 ms of 1 kHz toggles starting at the current cursor
        // position (which is now ≈ kFrameCount * cyclesPerSample).
        const uint64_t startCycle =
            static_cast<uint64_t>(kFrameCount * POM2_CPU_CLOCK_HZ / kSampleRate);
        driveToggles(spk, 1000.0, 0.05, startCycle);

        // Render enough buffers to consume those toggles (~50 ms ≈ 2 buffers).
        std::vector<float> all;
        for (int b = 0; b < 3; ++b) {
            runOneBuffer(spk, buf);
            all.insert(all.end(), buf.begin(), buf.end());
        }

        // Should see significant amplitude swing — at least one sample
        // above +0.05 AND at least one below -0.05.
        bool sawPos = false, sawNeg = false;
        for (float v : all) {
            if (v >  0.05f) sawPos = true;
            if (v < -0.05f) sawNeg = true;
        }
        assert(sawPos && sawNeg);
    }

    // ── Case 3: reset() clears state ────────────────────────────────────
    {
        SpeakerDevice spk;
        spk.setSampleRate(kSampleRate);
        for (int i = 0; i < 100; ++i) spk.recordToggle(i * 100);
        assert(spk.getQueuedEventCount() > 0);
        spk.reset();
        assert(spk.getQueuedEventCount() == 0);
        runOneBuffer(spk, buf);
        for (int i = 0; i < kFrameCount; ++i) {
            assert(std::fabs(buf[i]) < 0.05f);
        }
    }

    // ── Case 4: muted output ────────────────────────────────────────────
    {
        SpeakerDevice spk;
        spk.setSampleRate(kSampleRate);
        spk.setMuted(true);
        driveToggles(spk, 1000.0, 0.05, 0);
        runOneBuffer(spk, buf);
        for (int i = 0; i < kFrameCount; ++i) {
            assert(buf[i] == 0.0f);
        }
    }

    std::printf("Speaker smoke: OK (silence, square synth, reset, mute)\n");
    return 0;
}

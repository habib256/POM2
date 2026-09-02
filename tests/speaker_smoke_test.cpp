// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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

    // ── Case 5: pause/resume — consumer-ahead re-anchor ─────────────────
    // The audio callback keeps running while the machine is paused
    // (Mode::Stopped parks the worker but nothing stops the ma_device), so
    // the reconstruction cursor keeps consuming cycles while the CPU cycle
    // counter freezes. On resume every toggle is stamped far behind the
    // cursor; without the backward re-anchor in fillAudioBuffer the stale
    // purge ate the whole resumed stream (a net parity flip per buffer) and
    // speaker audio stayed clicks/silence for the rest of the session.
    {
        SpeakerDevice spk;
        spk.setSampleRate(kSampleRate);

        // A short burst before the pause, fully consumed.
        driveToggles(spk, 1000.0, 0.02, 0);
        runOneBuffer(spk, buf);
        runOneBuffer(spk, buf);

        // ~2 s pause: ~86 silent callbacks advance the cursor ~2M cycles
        // while the producer's clock is frozen.
        for (int b = 0; b < 86; ++b) runOneBuffer(spk, buf);

        // Resume: the producer continues from its FROZEN cycle counter.
        const uint64_t resumeCycle =
            static_cast<uint64_t>(0.03 * POM2_CPU_CLOCK_HZ);
        driveToggles(spk, 1000.0, 0.10, resumeCycle);

        // kCatchUpSecs of re-anchor lead + 100 ms of tone ≈ 0.2 s → give it
        // 12 buffers (~280 ms) and require a real square-wave swing.
        bool sawPos = false, sawNeg = false;
        for (int b = 0; b < 12; ++b) {
            runOneBuffer(spk, buf);
            for (float v : buf) {
                if (v >  0.05f) sawPos = true;
                if (v < -0.05f) sawNeg = true;
            }
        }
        assert(sawPos && sawNeg &&
               "resumed toggles were purged as stale after a pause");
    }

    std::printf("Speaker smoke: OK (silence, square synth, reset, mute, "
                "pause/resume)\n");
    return 0;
}

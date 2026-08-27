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

// Central audio output (miniaudio). Mixes registered AudioSource instances
// into the device's INTERLEAVED STEREO float32 output buffer. Ported from
// POM1's AudioDevice — POM2 is desktop-only for now (no WASM target), so the
// Web Audio fallback is dropped.
//
// ── Why the bus is stereo (2026-08-01) ───────────────────────────────────
// The Mockingboard is a stereo card: MAME gives it one 2-channel speaker
// with AY1 on channel 0 and AY2 on channel 1 (`a2mockingboard.cpp:159-165`),
// and the Phasor gets a second one, left = AY1+AY2, right = AY3+AY4
// (`:192-208`). Summing both chips to one channel destroyed the deliberate
// A/B/C pan that music like Digidream 1 writes.
//
// The MONO CONTRACT IS UNCHANGED. `fillAudioBuffer` still hands a source a
// single-channel buffer; the mixer places it with `AudioSource::pan`, whose
// default (centre) gives unity gain on BOTH channels — so speaker, cassette
// and floppy sounds keep exactly the level they had. A source that is
// natively stereo overrides `fillAudioBufferStereo` instead, which lets the
// cards migrate one at a time.

#ifndef POM2_AUDIO_DEVICE_H
#define POM2_AUDIO_DEVICE_H

#include <algorithm>   // std::max — getMasterPeak folds the two channels
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

struct ma_device;

class AudioSource
{
public:
    virtual ~AudioSource() = default;
    /// Fill `output` with `frameCount` mono float32 samples. Called from
    /// the audio callback thread — must be fast and thread-safe.
    /// The mixer places the result in the stereo field using `pan`.
    virtual void fillAudioBuffer(float* output, int frameCount) = 0;

    /// Optional native-stereo path, for sources whose hardware really has
    /// two outputs (Mockingboard: AY1 left / AY2 right; Phasor: one AY
    /// pair per side). `left` and `right` are separate `frameCount`-sample
    /// planes, both pre-zeroed by the mixer, and `pan` is NOT applied —
    /// the source owns its own placement, exactly as the card's wiring
    /// does. Return true when filled; the default returns false and the
    /// mixer falls back to the mono path above, so existing sources need
    /// no change.
    virtual bool fillAudioBufferStereo(float* /*left*/, float* /*right*/,
                                       int /*frameCount*/) { return false; }

    /// Post-fill peak abs amplitude of the last buffer this source
    /// produced, with a short release envelope (decays ~85% per 5 ms
    /// buffer → invisible after ~100 ms of silence). Updated by
    /// AudioDevice::mixSources right after calling fillAudioBuffer; the
    /// UI mixer panel reads it to draw a small level meter so users can
    /// immediately confirm a channel is alive. Stored on AudioSource
    /// (not AudioDevice) so it survives source list reshuffles and
    /// avoids a parallel-vector lookup on the audio thread.
    std::atomic<float> lastBufferPeak{0.0f};

    /// Stereo placement for the MONO path, -1 = hard left, 0 = centre,
    /// +1 = hard right. A balance law, not a constant-power one: the
    /// centre position is unity on both channels, so making the bus
    /// stereo did not change the level of any existing mono source.
    /// Ignored by sources that implement `fillAudioBufferStereo`.
    std::atomic<float> pan{0.0f};
};

/// Optional mixin for sources whose synthesis depends on the host audio
/// rate. AudioDevice::addSource auto-calls setSampleRate(actualSampleRate)
/// on any source that also inherits this — defensive against forgetting
/// the explicit call after a hot-plug. Existing call sites keep their
/// manual setSampleRate; the auto path just guarantees the source is
/// configured before the first fillAudioBuffer.
class RateAware
{
public:
    virtual ~RateAware() = default;
    virtual void setSampleRate(uint32_t hz) = 0;
};

class AudioDevice
{
public:
    static constexpr uint32_t kSampleRate = 44100;
    /// Interleaved stereo. `mixSources` writes frameCount * kChannels
    /// floats — callers sizing a buffer must account for both.
    static constexpr uint32_t kChannels   = 2;

    AudioDevice();
    ~AudioDevice();

    void addSource(AudioSource* source);
    void removeSource(AudioSource* source);

    bool isAvailable() const { return audioAvailable; }

    /// Sample rate negotiated with the OS device. miniaudio frequently
    /// picks 48 kHz on Apple Silicon even when 44.1 kHz is requested;
    /// cycle-driven sources (cassette, future speaker) MUST use this rate
    /// or their playback drifts by the rate ratio.
    uint32_t getActualSampleRate() const { return actualSampleRate; }

    /// Master gain applied after per-source mix, before clamp. Range
    /// [0, 2]. UI mixer panel binds directly to these atomics.
    void  setMasterVolume(float v);
    float getMasterVolume() const { return masterVolume_.load(std::memory_order_relaxed); }
    void  setMasterMuted(bool m) { masterMuted_.store(m, std::memory_order_relaxed); }
    bool  isMasterMuted() const  { return masterMuted_.load(std::memory_order_relaxed); }

    /// Fold the stereo mix back down to a centred mono image
    /// (0.5 * (L + R) on both channels — the exact signal the bus
    /// produced before it went stereo). For mono playback hardware, and
    /// for users who would rather not have a single-AY tune (Digidream 2
    /// never touches chip 2) come out of the left speaker only. Off by
    /// default: the hardware IS stereo, and that is what MAME renders.
    void setMonoDownmix(bool m) { monoDownmix_.store(m, std::memory_order_relaxed); }
    bool isMonoDownmix() const  { return monoDownmix_.load(std::memory_order_relaxed); }

    /// Post-clamp peak abs amplitude of the last mixed buffer, same
    /// release envelope as AudioSource::lastBufferPeak. Mirrors what
    /// the OS actually heard, so the master meter reflects clipping
    /// (saturates at 1.0). Read by the mixer panel. `getMasterPeak` is
    /// the louder of the two channels, so a single meter still shows
    /// clipping on either side.
    float getMasterPeakL() const { return masterPeakL_.load(std::memory_order_relaxed); }
    float getMasterPeakR() const { return masterPeakR_.load(std::memory_order_relaxed); }
    float getMasterPeak() const
    {
        return std::max(masterPeakL_.load(std::memory_order_relaxed),
                        masterPeakR_.load(std::memory_order_relaxed));
    }

    /// Mix all registered sources into `output` (clamped to [-1, +1]).
    /// Called from miniaudio's data callback. `output` is INTERLEAVED
    /// STEREO and must hold `frameCount * kChannels` floats.
    void mixSources(float* output, int frameCount);

private:
    bool initAudio();
    void shutdownAudio();

    std::vector<AudioSource*> sources;
    mutable std::mutex sourcesMutex;
    // Per-source planar scratch. `tmpBuf` alone in the mono days; the
    // stereo path needs a second plane, and the mono path still fills
    // only tmpBuf (which the pan law then spreads across both).
    std::vector<float> tmpBuf;
    std::vector<float> tmpBufR;
    bool audioAvailable = false;
    uint32_t actualSampleRate = kSampleRate;

    std::atomic<float> masterVolume_{1.0f};
    std::atomic<bool>  masterMuted_{false};
    std::atomic<bool>  monoDownmix_{false};
    std::atomic<float> masterPeakL_{0.0f};
    std::atomic<float> masterPeakR_{0.0f};

    struct MaDeviceDeleter { void operator()(ma_device* d) const noexcept; };
    std::unique_ptr<ma_device, MaDeviceDeleter> device;
    static void audioDataCallback(ma_device* pDevice, void* pOutput,
                                  const void* pInput, uint32_t frameCount);
    static void miniaudioLogCallback(void* pUserData, uint32_t level,
                                     const char* pMessage);
};

#endif // POM2_AUDIO_DEVICE_H

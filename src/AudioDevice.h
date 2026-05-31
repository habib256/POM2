// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Central audio output (miniaudio). Mixes registered AudioSource instances
// into the device's mono float32 output buffer. Ported from POM1's
// AudioDevice — POM2 is desktop-only for now (no WASM target), so the
// Web Audio fallback is dropped.

#ifndef POM2_AUDIO_DEVICE_H
#define POM2_AUDIO_DEVICE_H

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
    virtual void fillAudioBuffer(float* output, int frameCount) = 0;

    /// Post-fill peak abs amplitude of the last buffer this source
    /// produced, with a short release envelope (decays ~85% per 5 ms
    /// buffer → invisible after ~100 ms of silence). Updated by
    /// AudioDevice::mixSources right after calling fillAudioBuffer; the
    /// UI mixer panel reads it to draw a small level meter so users can
    /// immediately confirm a channel is alive. Stored on AudioSource
    /// (not AudioDevice) so it survives source list reshuffles and
    /// avoids a parallel-vector lookup on the audio thread.
    std::atomic<float> lastBufferPeak{0.0f};
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

    /// Post-clamp peak abs amplitude of the last mixed buffer, same
    /// release envelope as AudioSource::lastBufferPeak. Mirrors what
    /// the OS actually heard, so the master meter reflects clipping
    /// (saturates at 1.0). Read by the mixer panel.
    float getMasterPeak() const { return masterPeak_.load(std::memory_order_relaxed); }

    /// Mix all registered sources into `output` (clamped to [-1, +1]).
    /// Called from miniaudio's data callback.
    void mixSources(float* output, int frameCount);

private:
    bool initAudio();
    void shutdownAudio();

    std::vector<AudioSource*> sources;
    mutable std::mutex sourcesMutex;
    std::vector<float> tmpBuf;
    bool audioAvailable = false;
    uint32_t actualSampleRate = kSampleRate;

    std::atomic<float> masterVolume_{1.0f};
    std::atomic<bool>  masterMuted_{false};
    std::atomic<float> masterPeak_{0.0f};

    struct MaDeviceDeleter { void operator()(ma_device* d) const noexcept; };
    std::unique_ptr<ma_device, MaDeviceDeleter> device;
    static void audioDataCallback(ma_device* pDevice, void* pOutput,
                                  const void* pInput, uint32_t frameCount);
    static void miniaudioLogCallback(void* pUserData, uint32_t level,
                                     const char* pMessage);
};

#endif // POM2_AUDIO_DEVICE_H

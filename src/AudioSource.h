// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Host-neutral audio contracts shared by emulated devices and runtime mixers.
// This header deliberately contains no miniaudio or operating-system backend.

#ifndef POM2_AUDIO_SOURCE_H
#define POM2_AUDIO_SOURCE_H

#include <atomic>
#include <cstdint>

class AudioSource
{
public:
    static constexpr std::uint32_t kDefaultSampleRate = 44100;

    virtual ~AudioSource() = default;
    /// Fill `output` with `frameCount` mono float32 samples. Called from
    /// the audio callback thread — must be fast and thread-safe.
    virtual void fillAudioBuffer(float* output, int frameCount) = 0;

    /// Optional native-stereo path. Return true when both planes were filled;
    /// the default lets the runtime mixer fall back to the mono path.
    virtual bool fillAudioBufferStereo(float* /*left*/, float* /*right*/,
                                       int /*frameCount*/) { return false; }

    /// Runtime-owned metering and mono-source placement controls.
    std::atomic<float> lastBufferPeak{0.0f};
    std::atomic<float> pan{0.0f};
};

/// Optional contract for sources whose synthesis depends on host PCM rate.
class RateAware
{
public:
    virtual ~RateAware() = default;
    virtual void setSampleRate(std::uint32_t hz) = 0;
};

#endif // POM2_AUDIO_SOURCE_H

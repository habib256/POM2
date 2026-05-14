// POM2 Apple II Emulator
// Copyright (C) 2026

#include "AudioDevice.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>

// Vorbis backend for miniaudio: including stb_vorbis.c in this TU defines
// STB_VORBIS_INCLUDE_STB_VORBIS_H, which lets miniaudio find its decoder.
// stb_vorbis raises benign warnings under -Wall — silence them locally.
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wall"
#pragma clang diagnostic ignored "-Wextra"
#pragma clang diagnostic ignored "-Wshadow"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "third_party/stb_vorbis.c"
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#endif

// GCC's -Wstringop-overflow trips a false positive on miniaudio's atomic
// intrinsics (ma_atomic_load_64 on &pSound->seekTarget). Silence locally;
// no other warnings appear in the miniaudio TU at our level.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
#define MINIAUDIO_IMPLEMENTATION
#include "third_party/miniaudio.h"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#if defined(_WIN32)
#  ifdef min
#    undef min
#  endif
#  ifdef max
#    undef max
#  endif
#endif

void AudioDevice::mixSources(float* output, int frameCount)
{
    std::memset(output, 0, static_cast<size_t>(frameCount) * sizeof(float));

    std::lock_guard<std::mutex> lock(sourcesMutex);
    if (static_cast<int>(tmpBuf.size()) < frameCount)
        tmpBuf.resize(static_cast<size_t>(frameCount));

    for (AudioSource* src : sources) {
        // Zero the temp buffer before each source. The AudioSource
        // contract is *either* assign (Speaker, Cassette, Mockingboard
        // all do `output[i] = ...`) *or* mix additively starting from
        // silence (FloppySoundDevice). Without this memset, when the
        // sources iterate in order [A, B] and A writes its signal into
        // tmpBuf, an additive source B would see A's samples and add on
        // top — A then gets counted twice in output (A's pass already
        // added them once), producing audible doubling whenever two
        // sources are simultaneously active. Symptom seen during cold
        // boot: speaker bell beep + Disk II spin-up overlapped, giving
        // a "horrible" composite. The cost is one extra memset per
        // source per ~5 ms buffer — negligible.
        std::memset(tmpBuf.data(), 0,
                    static_cast<size_t>(frameCount) * sizeof(float));
        src->fillAudioBuffer(tmpBuf.data(), frameCount);
        for (int i = 0; i < frameCount; ++i)
            output[i] += tmpBuf[i];
    }

    for (int i = 0; i < frameCount; ++i)
        output[i] = std::max(-1.0f, std::min(1.0f, output[i]));
}

void AudioDevice::addSource(AudioSource* source)
{
    if (!source) return;
    std::lock_guard<std::mutex> lock(sourcesMutex);
    sources.push_back(source);
}

void AudioDevice::removeSource(AudioSource* source)
{
    std::lock_guard<std::mutex> lock(sourcesMutex);
    sources.erase(std::remove(sources.begin(), sources.end(), source), sources.end());
}

void AudioDevice::audioDataCallback(ma_device* pDevice, void* pOutput,
                                    const void* /*pInput*/, uint32_t frameCount)
{
    AudioDevice* self = static_cast<AudioDevice*>(pDevice->pUserData);
    float* output = static_cast<float*>(pOutput);
    if (self == nullptr) {
        std::fill(output, output + frameCount, 0.0f);
        return;
    }
    self->mixSources(output, static_cast<int>(frameCount));
}

AudioDevice::AudioDevice()
{
    initAudio();
}

AudioDevice::~AudioDevice()
{
    shutdownAudio();
}

bool AudioDevice::initAudio()
{
    shutdownAudio();

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format    = ma_format_f32;
    config.playback.channels  = 1;
    config.sampleRate         = kSampleRate;
    config.periodSizeInFrames = 256;
    config.periods            = 3;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.dataCallback       = &AudioDevice::audioDataCallback;
    config.pUserData          = this;

    ma_device* raw = new ma_device();
    if (ma_device_init(nullptr, &config, raw) != MA_SUCCESS) {
        delete raw;
        audioAvailable = false;
        pom2::log().warn("Audio", "ma_device_init failed — audio disabled");
        return false;
    }
    if (ma_device_start(raw) != MA_SUCCESS) {
        ma_device_uninit(raw);
        delete raw;
        audioAvailable = false;
        pom2::log().warn("Audio", "ma_device_start failed — audio disabled");
        return false;
    }

    actualSampleRate = raw->sampleRate;
    pom2::log().info("Audio",
        std::string("miniaudio ready: requested ") + std::to_string(kSampleRate) +
        " Hz, got " + std::to_string(actualSampleRate) + " Hz");

    device.reset(raw);
    audioAvailable = true;
    return true;
}

void AudioDevice::shutdownAudio()
{
    // device.reset() -> MaDeviceDeleter -> ma_device_uninit synchronously
    // drains the callback, so by the time we clear sources no audio thread
    // is racing with us.
    device.reset();
    audioAvailable = false;
    {
        std::lock_guard<std::mutex> lock(sourcesMutex);
        sources.clear();
    }
}

void AudioDevice::MaDeviceDeleter::operator()(ma_device* d) const noexcept
{
    if (!d) return;
    ma_device_uninit(d);
    delete d;
}

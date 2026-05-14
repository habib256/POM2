// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Apple II built-in cassette interface, ported from POM1's ACI.

#include "CassetteDevice.h"

// miniaudio is compiled via AudioDevice.cpp (MINIAUDIO_IMPLEMENTATION lives
// there). We only need the function prototypes here.
#include "third_party/miniaudio.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <vector>

namespace {

// .aci magic — kept identical to POM1 so cassettes recorded on either
// emulator can round-trip. The format (8-byte magic + 1 version byte +
// 1 initial-level byte + 2 padding + 4 LE count + count×4 LE durations)
// is structural to the pulse-stream representation, not Apple-specific.
constexpr char kAciMagic[] = "POM1ACI1";
constexpr uint32_t kMaxRealtimeGapCycles = 50000;
constexpr uint32_t kAudioRampInSamples = 64;

uint16_t readLe16(const uint8_t* d)
{
    return static_cast<uint16_t>(d[0] | (static_cast<uint16_t>(d[1]) << 8));
}

uint32_t readLe32(const uint8_t* d)
{
    return static_cast<uint32_t>(d[0]) |
           (static_cast<uint32_t>(d[1]) << 8) |
           (static_cast<uint32_t>(d[2]) << 16) |
           (static_cast<uint32_t>(d[3]) << 24);
}

void writeLe16(std::ofstream& f, uint16_t v)
{
    const uint8_t b[2] = { static_cast<uint8_t>(v & 0xFF),
                           static_cast<uint8_t>((v >> 8) & 0xFF) };
    f.write(reinterpret_cast<const char*>(b), 2);
}

void writeLe32(std::ofstream& f, uint32_t v)
{
    const uint8_t b[4] = { static_cast<uint8_t>(v & 0xFF),
                           static_cast<uint8_t>((v >> 8) & 0xFF),
                           static_cast<uint8_t>((v >> 16) & 0xFF),
                           static_cast<uint8_t>((v >> 24) & 0xFF) };
    f.write(reinterpret_cast<const char*>(b), 4);
}

std::string lowerExtension(const std::string& path)
{
    std::string ext = std::filesystem::path(path).extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // namespace

std::string CassetteDevice::lookupTapeInfo(const std::string& path)
{
    namespace fs = std::filesystem;
    const fs::path tapePath(path);
    const fs::path dir = tapePath.parent_path();
    if (dir.empty()) return {};

    const fs::path infoFile = dir / "tapeinfo.txt";
    std::ifstream file(infoFile);
    if (!file.is_open()) return {};

    const std::string baseName = tapePath.filename().string();
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!val.empty() && val.front() == ' ') val.erase(val.begin());
        if (key == baseName) return val;
    }
    return {};
}

CassetteDevice::CassetteDevice()
{
    reset();
}

void CassetteDevice::fillAudioBuffer(float* output, int frameCount)
{
    // Stream mode — direct decode + resample via miniaudio.
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        if (!audioStreamDecoderOpen || !playbackActive ||
            playbackPaused.load(std::memory_order_acquire)) {
            std::fill_n(output, frameCount, 0.0f);
            return;
        }
        ma_uint64 framesRead = 0;
        ma_decoder_read_pcm_frames(&audioStreamDecoder, output,
                                   static_cast<ma_uint64>(frameCount), &framesRead);
        audioStreamCursor += framesRead;

        constexpr float kStreamGain = 0.71f;
        const float vol = volume.load(std::memory_order_relaxed);
        const int consumed = static_cast<int>(framesRead);
        for (int i = 0; i < consumed; ++i) {
            output[i] *= kStreamGain * vol;
            if (audioRampInSamplesRemaining > 0) {
                const float ramp = 1.0f - (static_cast<float>(audioRampInSamplesRemaining) /
                                           static_cast<float>(kAudioRampInSamples));
                output[i] *= ramp;
                audioRampInSamplesRemaining--;
            }
        }
        if (consumed < frameCount) {
            std::fill_n(output + consumed, frameCount - consumed, 0.0f);
            if (framesRead == 0) playbackActive = false;
        }
        // Mix the mode-transition clunk on top.
        {
            std::lock_guard<std::mutex> clickLock(audioMutex);
            if (clickCursor < clickBuffer.size()) {
                const int mix = std::min<int>(frameCount,
                    static_cast<int>(clickBuffer.size() - clickCursor));
                for (int i = 0; i < mix; ++i) {
                    output[i] += clickBuffer[clickCursor++] * vol;
                }
            }
        }
        return;
    }

    // Pulse mode — drain the segment queue at the device sample rate.
    static constexpr float kFilterAlpha = 0.33f;
    std::lock_guard<std::mutex> lock(audioMutex);
    if (playbackPaused.load(std::memory_order_acquire)) {
        std::fill_n(output, frameCount, 0.0f);
        audioPlaybackSample = 0.0f;
        return;
    }
    const float vol = volume.load(std::memory_order_relaxed);
    for (int i = 0; i < frameCount; ++i) {
        float targetSample = 0.0f;
        if (!audioQueue.empty()) {
            targetSample = audioQueue.front().sampleValue;
            if (audioQueue.front().remainingSamples > 0)
                audioQueue.front().remainingSamples--;
            if (audioQueue.front().remainingSamples == 0)
                audioQueue.pop_front();
        }
        if (audioRampInSamplesRemaining > 0) {
            const float ramp = 1.0f - (static_cast<float>(audioRampInSamplesRemaining) /
                                       static_cast<float>(kAudioRampInSamples));
            targetSample *= ramp;
            audioRampInSamplesRemaining--;
        }
        audioPlaybackSample += (targetSample - audioPlaybackSample) * kFilterAlpha;
        float s = audioPlaybackSample * vol;
        if (clickCursor < clickBuffer.size())
            s += clickBuffer[clickCursor++] * vol;
        output[i] = s;
    }
}

double CassetteDevice::getQueuedAudioSeconds() const
{
    std::lock_guard<std::mutex> lock(audioMutex);
    uint64_t queued = 0;
    for (const auto& seg : audioQueue) queued += seg.remainingSamples;
    return static_cast<double>(queued) / static_cast<double>(audioOutputSampleRate);
}

void CassetteDevice::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    volume.store(v, std::memory_order_relaxed);
}

void CassetteDevice::resetPlaybackState()
{
    // Always leave the deck DISARMED. Arming is the user's responsibility
    // (PLAY button only).
    playbackArmed   = false;
    playbackActive  = false;
    playbackIndex   = 0;
    cyclesUntilInputToggle = 0;
    inputLevel      = loadedInitialLevel;
    rewinding       = false;
    rewCarryCycles  = 0;
    lastTapeInputCycle = currentCycle;
}

void CassetteDevice::clearLiveAudioState()
{
    std::lock_guard<std::mutex> lock(audioMutex);
    audioSampleRemainder = 0.0;
    audioPlaybackSample  = 0.0f;
    audioRampInSamplesRemaining = kAudioRampInSamples;
    audioQueue.clear();
}

void CassetteDevice::stopPulseAudio()
{
    clearLiveAudioState();
}

void CassetteDevice::playMechanicalClick()
{
    // ~70 ms damped thud + noise burst, mixed on top of the deck output.
    std::lock_guard<std::mutex> lock(audioMutex);
    const uint32_t rate = std::max<uint32_t>(1, audioOutputSampleRate);
    const uint32_t durSamples = rate / 14;
    clickBuffer.assign(durSamples, 0.0f);
    uint32_t lcg = 0xC7E5A5B7u;
    constexpr float kTwoPi = 6.28318530718f;
    for (uint32_t i = 0; i < durSamples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(rate);
        const float attack = std::min(1.0f, t * 400.0f);
        const float decay  = std::exp(-t * 30.0f);
        lcg = lcg * 1664525u + 1013904223u;
        const float noise = (static_cast<float>(static_cast<int32_t>(lcg)) / 2147483648.0f);
        const float thud = std::sin(kTwoPi * 95.0f * t);
        const float tick = std::sin(kTwoPi * 1300.0f * t) * std::exp(-t * 120.0f);
        clickBuffer[i] = (0.45f * thud + 0.30f * tick + 0.25f * noise) * attack * decay * 0.35f;
    }
    clickCursor = 0;
}

void CassetteDevice::fireClickIfModeChanged()
{
    const DeckMode m = getDeckMode();
    if (m == lastDeckMode) return;
    lastDeckMode = m;
    playMechanicalClick();
}

void CassetteDevice::reset()
{
    currentCycle = 0;
    outputLevel  = false;
    recordedInitialLevel = false;
    lastOutputToggleCycle = 0;
    recordedDurations.clear();
    playbackPaused.store(false, std::memory_order_release);
    resetPlaybackState();
    stopPulseAudio();
}

void CassetteDevice::resetCpuSide()
{
    // Apple II hard-reset clobbers the cassette OUTPUT flip-flop and the
    // CPU-cycle timebase only. Tape position, recording buffer and
    // mechanical state survive — a real deck doesn't rewind because the
    // computer was reset.
    currentCycle = 0;
    outputLevel  = false;
    lastOutputToggleCycle = 0;
}

void CassetteDevice::setPlaybackPaused(bool paused)
{
    const bool prev = playbackPaused.exchange(paused, std::memory_order_acq_rel);
    if (prev == paused || paused) return;
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        audioRampInSamplesRemaining = kAudioRampInSamples;
    } else {
        std::lock_guard<std::mutex> lock(audioMutex);
        audioRampInSamplesRemaining = kAudioRampInSamples;
    }
}

void CassetteDevice::seekRelativeSeconds(double deltaSeconds)
{
    if (!audioStreamMode) return;
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (!audioStreamDecoderOpen || audioOutputSampleRate == 0) return;

    const int64_t rate = static_cast<int64_t>(audioOutputSampleRate);
    int64_t newFrame = static_cast<int64_t>(audioStreamCursor) +
                       static_cast<int64_t>(std::llround(deltaSeconds * static_cast<double>(rate)));
    if (newFrame < 0) newFrame = 0;
    if (audioStreamTotalFrames > 0 &&
        newFrame >= static_cast<int64_t>(audioStreamTotalFrames)) {
        newFrame = static_cast<int64_t>(audioStreamTotalFrames) - 1;
    }
    if (ma_decoder_seek_to_pcm_frame(&audioStreamDecoder,
                                     static_cast<ma_uint64>(newFrame)) != MA_SUCCESS)
        return;
    audioStreamCursor = static_cast<uint64_t>(newFrame);
    audioRampInSamplesRemaining = kAudioRampInSamples;
}

double CassetteDevice::getPlaybackPositionSeconds() const
{
    if (!audioStreamMode) return 0.0;
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (audioOutputSampleRate == 0) return 0.0;
    return static_cast<double>(audioStreamCursor) / static_cast<double>(audioOutputSampleRate);
}

double CassetteDevice::getPlaybackTotalSeconds() const
{
    if (!audioStreamMode) return 0.0;
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (audioOutputSampleRate == 0) return 0.0;
    return static_cast<double>(audioStreamTotalFrames) / static_cast<double>(audioOutputSampleRate);
}

void CassetteDevice::queueAudioSegment(uint32_t cycles, bool level)
{
    if (!audioAvailable || cycles == 0) return;

    const double totalSamples = audioSampleRemainder +
        (static_cast<double>(cycles) * static_cast<double>(audioOutputSampleRate) /
         static_cast<double>(kRealtimeAudioTimebaseHz));
    const uint32_t sampleCount = static_cast<uint32_t>(totalSamples);
    audioSampleRemainder = totalSamples - static_cast<double>(sampleCount);

    if (sampleCount == 0) return;

    const float sampleValue = level ? 0.22f : -0.22f;
    std::lock_guard<std::mutex> lock(audioMutex);
    if (!audioQueue.empty() && audioQueue.back().sampleValue == sampleValue) {
        audioQueue.back().remainingSamples += sampleCount;
    } else {
        audioQueue.push_back({sampleCount, sampleValue});
    }

    static constexpr size_t kMaxQueuedSegments = 8192;
    while (audioQueue.size() > kMaxQueuedSegments) audioQueue.pop_front();
}

void CassetteDevice::advancePlayback(uint32_t cycles)
{
    if (rewinding) { advanceRewind(cycles); return; }
    if (!playbackActive || loadedDurations.empty() || cycles == 0) return;
    if (playbackPaused.load(std::memory_order_acquire)) return;

    uint64_t remaining = cycles;
    while (remaining > 0 && playbackActive) {
        if (cyclesUntilInputToggle == 0) {
            if (playbackIndex >= loadedDurations.size()) {
                playbackActive = false;
                break;
            }
            cyclesUntilInputToggle = std::max<uint32_t>(1, loadedDurations[playbackIndex++]);
        }
        if (remaining < cyclesUntilInputToggle) {
            cyclesUntilInputToggle -= remaining;
            break;
        }
        remaining -= cyclesUntilInputToggle;
        queueAudioSegment(std::max<uint32_t>(1, loadedDurations[playbackIndex - 1]),
                          inputLevel);
        cyclesUntilInputToggle = 0;
        inputLevel = !inputLevel;
        if (playbackIndex >= loadedDurations.size()) playbackActive = false;
    }
}

void CassetteDevice::advanceRewind(uint32_t cycles)
{
    if (playbackPaused.load(std::memory_order_acquire)) return;
    if (!loadedTapeReady || loadedDurations.empty() || playbackIndex == 0) {
        rewinding = false;
        rewCarryCycles = 0;
        resetPlaybackState();
        return;
    }
    uint64_t budget = static_cast<uint64_t>(cycles) * kRewSpeedFactor + rewCarryCycles;
    while (playbackIndex > 0) {
        const uint32_t segDur = std::max<uint32_t>(1, loadedDurations[playbackIndex - 1]);
        if (budget < segDur) {
            rewCarryCycles = budget;
            return;
        }
        budget -= segDur;
        --playbackIndex;
        inputLevel = !inputLevel;
    }
    resetPlaybackState();
    clearLiveAudioState();
}

void CassetteDevice::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    advancePlayback(static_cast<uint32_t>(cycles));
    currentCycle += static_cast<uint32_t>(cycles);
}

void CassetteDevice::beginRecordingIfNeeded()
{
    if (lastOutputToggleCycle == 0 && recordedDurations.empty()) {
        clearLiveAudioState();
        recordedInitialLevel = outputLevel;
        lastOutputToggleCycle = currentCycle;
    }
}

uint8_t CassetteDevice::toggleOutput()
{
    beginRecordingIfNeeded();

    if (currentCycle > lastOutputToggleCycle) {
        const uint64_t delta = currentCycle - lastOutputToggleCycle;
        if (!(recordedDurations.empty() && delta == 0)) {
            const uint32_t clamped = static_cast<uint32_t>(std::max<uint64_t>(1, delta));
            recordedDurations.push_back(clamped);
            if (clamped > kMaxRealtimeGapCycles) {
                clearLiveAudioState();
            } else {
                queueAudioSegment(clamped, outputLevel);
            }
        }
    }

    lastOutputToggleCycle = currentCycle;
    outputLevel = !outputLevel;
    return outputLevel ? 0x80 : 0x00;
}

void CassetteDevice::armPlaybackAtStart()
{
    playbackIndex = 0;
    cyclesUntilInputToggle = 0;
    inputLevel = loadedInitialLevel;
    const bool becameActive = loadedTapeReady && !loadedDurations.empty();
    playbackActive = becameActive;
    playbackArmed  = false;
    clearLiveAudioState();
}

uint8_t CassetteDevice::readTapeInput()
{
    // During REW, freeze the tape input — REW flips inputLevel as it walks
    // playbackIndex backward, the read sees that frozen state.
    if (rewinding) return inputLevel ? 0x80 : 0x00;

    // Leader-preservation: if the Monitor's READ routine ($FEFD) hasn't
    // polled $C060 for a long while, we assume the user was typing in
    // BASIC / Monitor (which doesn't touch the cassette input). Rewind +
    // reactivate so the next READ sequence sees the leader from the start
    // and can synchronise to the 770 Hz sync tone.
    constexpr uint64_t kLeaderRewindGapCycles = POM2_CPU_CLOCK_HZ / 2;  // 500 ms
    const bool leaderRewind =
        loadedTapeReady && !loadedDurations.empty() && playbackIndex > 0 &&
        (currentCycle - lastTapeInputCycle) > kLeaderRewindGapCycles;
    if (leaderRewind || playbackArmed) armPlaybackAtStart();
    lastTapeInputCycle = currentCycle;
    return inputLevel ? 0x80 : 0x00;
}

void CassetteDevice::rewindTape()
{
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        if (audioStreamDecoderOpen) ma_decoder_seek_to_pcm_frame(&audioStreamDecoder, 0);
        audioStreamCursor = 0;
        playbackActive = false;
        clearLiveAudioState();
        return;
    }
    if (!loadedTapeReady || loadedDurations.empty() || playbackIndex == 0) {
        resetPlaybackState();
        stopPulseAudio();
        return;
    }
    rewinding = true;
    rewCarryCycles = 0;
    playbackActive = false;
    playbackArmed  = false;
    stopPulseAudio();
}

void CassetteDevice::playTape()
{
    if (!loadedTapeReady) return;
    playbackPaused.store(false, std::memory_order_release);
    if (audioStreamMode) {
        std::lock_guard<std::mutex> lock(audioStreamMutex);
        if (!audioStreamDecoderOpen) return;
        playbackActive = true;
        playbackArmed  = false;
        audioRampInSamplesRemaining = kAudioRampInSamples;
        return;
    }
    if (loadedDurations.empty()) return;
    // Pulse-mode PLAY: arms the deck but doesn't start advancing pulses.
    // The tape only begins moving when the Monitor's READ routine reads
    // $C060 for the first time (readTapeInput's armed→active transition).
    // Without this, advancePlayback would chew through the whole tape on
    // every CPU slice regardless of whether the routine was actually
    // reading — a 30 s tape would disappear in <1 s at MAX speed while
    // the user was typing the address range.
    resetPlaybackState();
    playbackArmed = true;
}

void CassetteDevice::stopTape()
{
    playbackActive = false;
    playbackArmed  = false;
    rewinding      = false;
    rewCarryCycles = 0;
    cyclesUntilInputToggle = 0;
    stopPulseAudio();
}

void CassetteDevice::ejectTape()
{
    closeAudioStream();
    stopPulseAudio();
    audioStreamMode = false;
    loadedDurations.clear();
    loadedTapePath.clear();
    loadInfo.clear();
    loadedTapeReady    = false;
    loadedInitialLevel = false;
    resetPlaybackState();
    fireClickIfModeChanged();
}

void CassetteDevice::clearRecordedTape()
{
    recordedDurations.clear();
    recordedInitialLevel = outputLevel;
    lastOutputToggleCycle = 0;
    clearLiveAudioState();
}

bool CassetteDevice::loadPlaybackDurations(std::vector<uint32_t> durations,
                                           bool initialLevel,
                                           const std::string& path)
{
    if (durations.empty()) {
        lastError = "Tape file does not contain any signal transitions";
        return false;
    }
    stopPulseAudio();
    loadedDurations    = std::move(durations);
    loadedInitialLevel = initialLevel;
    loadedTapePath     = path;
    loadedTapeReady    = true;
    audioStreamMode    = false;
    resetPlaybackState();
    fireClickIfModeChanged();
    lastError.clear();
    return true;
}

bool CassetteDevice::loadTape(const std::string& path)
{
    const std::string ext = lowerExtension(path);
    loadInfo = lookupTapeInfo(path);

    if (ext == ".aci") {
        closeAudioStream();
        audioStreamMode = false;
        return loadAciTape(path);
    }

    // Default: program tape via pulse extraction. .wav routes through the
    // hand-rolled WAV loader (no decoder dependency); the rest go through
    // miniaudio (mp3/ogg/flac).
    closeAudioStream();
    audioStreamMode = false;
    if (ext == ".wav") return loadWavTape(path);
    if (ext == ".mp3" || ext == ".ogg" || ext == ".flac")
        return loadMiniaudioTape(path);

    lastError = "Unsupported tape extension (expected .aci/.wav/.mp3/.ogg/.flac).";
    return false;
}

bool CassetteDevice::saveTape(const std::string& path) const
{
    const std::string ext = lowerExtension(path);
    if (ext == ".wav") return saveWavTape(path);
    return saveAciTape(path);
}

bool CassetteDevice::loadAciTape(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { lastError = "Cannot open tape file: " + path; return false; }

    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    if (bytes.size() < 16 || std::memcmp(bytes.data(), kAciMagic, 8) != 0) {
        lastError = "Invalid .aci tape file";
        return false;
    }
    if (bytes[8] != 1) {
        lastError = "Unsupported .aci tape version";
        return false;
    }
    const bool initialLevel = bytes[9] != 0;
    const uint32_t count = readLe32(bytes.data() + 12);
    if (bytes.size() < 16ull + static_cast<uint64_t>(count) * 4ull) {
        lastError = "Truncated .aci tape file";
        return false;
    }

    std::vector<uint32_t> durations;
    durations.reserve(count);
    size_t offset = 16;
    for (uint32_t i = 0; i < count; ++i) {
        durations.push_back(std::max<uint32_t>(1, readLe32(bytes.data() + offset)));
        offset += 4;
    }
    return loadPlaybackDurations(std::move(durations), initialLevel, path);
}

bool CassetteDevice::saveAciTape(const std::string& path) const
{
    if (recordedDurations.empty()) {
        lastError = "No cassette output has been recorded yet";
        return false;
    }
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) { lastError = "Cannot write tape file: " + path; return false; }

    file.write(kAciMagic, 8);
    file.put(1);
    file.put(recordedInitialLevel ? 1 : 0);
    file.put(0); file.put(0);
    writeLe32(file, static_cast<uint32_t>(recordedDurations.size()));
    for (uint32_t d : recordedDurations) writeLe32(file, d);

    lastError.clear();
    return true;
}

bool CassetteDevice::loadWavTape(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) { lastError = "Cannot open tape file: " + path; return false; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),
                                std::istreambuf_iterator<char>());
    if (bytes.size() < 44 || std::memcmp(bytes.data(), "RIFF", 4) != 0 ||
        std::memcmp(bytes.data() + 8, "WAVE", 4) != 0) {
        lastError = "Invalid WAV file";
        return false;
    }

    uint16_t audioFormat = 0, channels = 0, bitsPerSample = 0;
    uint32_t sampleRate = 0;
    const uint8_t* dataChunk = nullptr;
    uint32_t dataSize = 0;

    size_t offset = 12;
    while (offset + 8 <= bytes.size()) {
        const uint8_t* chunk = bytes.data() + offset;
        const uint32_t chunkSize = readLe32(chunk + 4);
        offset += 8;
        if (offset + chunkSize > bytes.size()) break;
        if (std::memcmp(chunk, "fmt ", 4) == 0 && chunkSize >= 16) {
            audioFormat   = readLe16(bytes.data() + offset + 0);
            channels      = readLe16(bytes.data() + offset + 2);
            sampleRate    = readLe32(bytes.data() + offset + 4);
            bitsPerSample = readLe16(bytes.data() + offset + 14);
        } else if (std::memcmp(chunk, "data", 4) == 0) {
            dataChunk = bytes.data() + offset;
            dataSize  = chunkSize;
        }
        offset += chunkSize + (chunkSize & 1u);
    }
    if (!dataChunk || channels == 0 || sampleRate == 0) {
        lastError = "WAV file is missing format or data chunks";
        return false;
    }
    if (audioFormat != 1 && audioFormat != 3) {
        lastError = "Unsupported WAV format (only PCM and float are supported)";
        return false;
    }
    const size_t bytesPerSample = bitsPerSample / 8;
    if (bytesPerSample == 0 || dataSize < bytesPerSample * channels) {
        lastError = "Unsupported WAV sample format";
        return false;
    }

    const size_t frames = dataSize / (bytesPerSample * channels);
    std::vector<float> samples;
    samples.reserve(frames);
    for (size_t f = 0; f < frames; ++f) {
        float mixed = 0.0f;
        for (uint16_t ch = 0; ch < channels; ++ch) {
            const uint8_t* p = dataChunk + (f * channels + ch) * bytesPerSample;
            float v = 0.0f;
            if      (audioFormat == 1 && bitsPerSample == 8)  v = (static_cast<int>(p[0]) - 128) / 128.0f;
            else if (audioFormat == 1 && bitsPerSample == 16) v = static_cast<float>(static_cast<int16_t>(readLe16(p))) / 32768.0f;
            else if (audioFormat == 3 && bitsPerSample == 32) std::memcpy(&v, p, 4);
            else { lastError = "Only WAV PCM 8/16-bit and float32 are supported"; return false; }
            mixed += v;
        }
        samples.push_back(mixed / static_cast<float>(channels));
    }

    std::vector<uint32_t> durations;
    bool initialLevel = false;
    if (!pcmToDurations(samples, sampleRate, durations, initialLevel, lastError))
        return false;
    return loadPlaybackDurations(std::move(durations), initialLevel, path);
}

bool CassetteDevice::pcmToDurations(const std::vector<float>& mono,
                                    uint32_t sampleRate,
                                    std::vector<uint32_t>& outDurations,
                                    bool& outInitialLevel,
                                    std::string& outErr)
{
    outDurations.clear();
    if (mono.empty())     { outErr = "Audio file does not contain samples"; return false; }
    if (sampleRate == 0)  { outErr = "Audio file has an invalid sample rate"; return false; }

    // Sign-based transition detection — matches MAME `apple2.cpp:362`
    // `(m_cassette->input() > 0.0 ? 0 : 0x80)`. Real hardware reads the
    // raw sign of the audio comparator at runtime, so faint rips
    // decode just as well as loud ones. The previous `±0.02` deadband
    // rejected any file whose first ~16 ms of sync gap had amplitude
    // under that floor (common with archived `wav` rips that were
    // normalised long after the master tape demagnetised).
    size_t firstActive = 0;
    // Skip exact-zero leading silence so we have a defined initial
    // sign. The first non-zero sample seeds outInitialLevel.
    while (firstActive < mono.size() && mono[firstActive] == 0.0f)
        ++firstActive;
    if (firstActive == mono.size()) {
        outErr = "Audio file does not contain a detectable cassette signal";
        return false;
    }

    outInitialLevel = mono[firstActive] > 0.0f;
    bool currentLevel = outInitialLevel;
    size_t lastTransition = firstActive;

    for (size_t i = firstActive + 1; i < mono.size(); ++i) {
        bool newLevel = currentLevel;
        if (mono[i] >  0.0f) newLevel = true;
        else if (mono[i] < 0.0f) newLevel = false;
        if (newLevel != currentLevel) {
            const size_t deltaSamples = i - lastTransition;
            const uint32_t cycles = std::max<uint32_t>(1, static_cast<uint32_t>(
                std::llround(static_cast<double>(deltaSamples) *
                             static_cast<double>(kTapeFileTimebaseHz) /
                             static_cast<double>(sampleRate))));
            outDurations.push_back(cycles);
            currentLevel    = newLevel;
            lastTransition  = i;
        }
    }
    return true;
}

bool CassetteDevice::loadAudioStream(const std::string& path)
{
    closeAudioStream();
    loadedDurations.clear();
    std::lock_guard<std::mutex> lock(audioStreamMutex);

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, audioOutputSampleRate);
    if (ma_decoder_init_file(path.c_str(), &cfg, &audioStreamDecoder) != MA_SUCCESS) {
        lastError = "Cannot decode audio: " + path;
        return false;
    }
    audioStreamDecoderOpen = true;
    audioStreamCursor      = 0;
    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&audioStreamDecoder, &total) != MA_SUCCESS) total = 0;
    audioStreamTotalFrames = total;
    audioStreamMode        = true;
    loadedTapePath         = path;
    loadedTapeReady        = true;
    playbackArmed          = false;
    playbackActive         = false;
    loadedInitialLevel     = false;
    stopPulseAudio();
    fireClickIfModeChanged();
    lastError.clear();
    return true;
}

void CassetteDevice::closeAudioStream()
{
    std::lock_guard<std::mutex> lock(audioStreamMutex);
    if (audioStreamDecoderOpen) {
        ma_decoder_uninit(&audioStreamDecoder);
        audioStreamDecoderOpen = false;
    }
    audioStreamCursor      = 0;
    audioStreamTotalFrames = 0;
}

bool CassetteDevice::loadMiniaudioTape(const std::string& path)
{
    // 30-minute cap — same as POM1.
    static constexpr uint64_t kMaxFrames = 30ull * 60ull * 96000ull;

    ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, 1, 0);
    ma_decoder decoder;
    if (ma_decoder_init_file(path.c_str(), &cfg, &decoder) != MA_SUCCESS) {
        lastError = "Cannot decode audio file: " + path;
        return false;
    }

    const uint32_t sampleRate = decoder.outputSampleRate;
    if (sampleRate == 0) {
        ma_decoder_uninit(&decoder);
        lastError = "Decoded audio reports an invalid sample rate";
        return false;
    }

    std::vector<float> samples;
    constexpr size_t kChunkFrames = 4096;
    float chunk[kChunkFrames];
    uint64_t totalFrames = 0;
    while (totalFrames < kMaxFrames) {
        ma_uint64 framesRead = 0;
        const ma_result r = ma_decoder_read_pcm_frames(&decoder, chunk, kChunkFrames, &framesRead);
        if (framesRead == 0) break;
        samples.insert(samples.end(), chunk, chunk + framesRead);
        totalFrames += framesRead;
        if (r != MA_SUCCESS) break;
    }
    ma_decoder_uninit(&decoder);

    if (totalFrames >= kMaxFrames) {
        lastError = "Audio file exceeds 30-minute tape limit";
        return false;
    }

    std::vector<uint32_t> durations;
    bool initialLevel = false;
    if (!pcmToDurations(samples, sampleRate, durations, initialLevel, lastError))
        return false;
    return loadPlaybackDurations(std::move(durations), initialLevel, path);
}

bool CassetteDevice::saveWavTape(const std::string& path) const
{
    if (recordedDurations.empty()) {
        lastError = "No cassette output has been recorded yet";
        return false;
    }
    std::vector<int16_t> pcm;
    bool level = recordedInitialLevel;
    for (uint32_t d : recordedDurations) {
        const uint32_t n = std::max<uint32_t>(1, static_cast<uint32_t>(
            std::llround(static_cast<double>(d) * static_cast<double>(kWavFileSampleRate) /
                         static_cast<double>(kTapeFileTimebaseHz))));
        const int16_t s = level ? 14000 : -14000;
        pcm.insert(pcm.end(), n, s);
        level = !level;
    }
    pcm.insert(pcm.end(), kWavFileSampleRate / 10, level ? 14000 : -14000);

    const size_t fullSize = pcm.size() * sizeof(int16_t);
    if (fullSize > UINT32_MAX - 36) {
        lastError = "Recording too large to save as WAV";
        return false;
    }
    const uint32_t dataSize = static_cast<uint32_t>(fullSize);
    const uint32_t riffSize = 36 + dataSize;

    std::ofstream f(path, std::ios::binary);
    if (!f.is_open()) { lastError = "Cannot write tape file: " + path; return false; }

    f.write("RIFF", 4); writeLe32(f, riffSize);
    f.write("WAVE", 4);
    f.write("fmt ", 4); writeLe32(f, 16);
    writeLe16(f, 1); writeLe16(f, 1);
    writeLe32(f, kWavFileSampleRate);
    writeLe32(f, kWavFileSampleRate * sizeof(int16_t));
    writeLe16(f, sizeof(int16_t)); writeLe16(f, 16);
    f.write("data", 4); writeLe32(f, dataSize);
    f.write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(dataSize));

    lastError.clear();
    return true;
}

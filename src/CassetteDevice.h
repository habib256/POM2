// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CassetteDevice — Apple II built-in cassette interface, ported from
// POM1's ACI implementation.
//
// Apple II MMIO (no card needed — built into the motherboard):
//   $C020   any access toggles the cassette OUTPUT flip-flop
//           (the Monitor's WRITE routine at $FECD calls "BIT $C020"
//            to drive the cassette write line.)
//   $C060   read returns bit-7 = sign of the cassette INPUT comparator
//           (the Monitor's READ routine at $FEFD reads $C060 in a tight
//            loop and times zero-crossings to recover the bit stream.)
//
// The pulse format (zero-crossing audio with 770 Hz sync, 1 kHz "1",
// 2 kHz "0") is structurally identical to the Apple-1 ACI tape format,
// just with different timing constants — POM1's pulse-extraction core
// (`pcmToDurations`, miniaudio decoders, audio queue, REW state machine)
// ports verbatim with the I/O addresses re-wired.
//
// Two playback modes (mirrored from POM1):
//   * PROGRAM TAPE — user loaded an .aci/.wav/.mp3/.ogg and the Apple II
//                    READ routine will poll $C060 to decode the data.
//                    Pulse durations are clocked out at the CPU rate.
//   * AUDIO STREAM — user dropped in a long mp3/ogg as background audio.
//                    miniaudio decodes + resamples; pulses don't drive
//                    $C060 (the bytes wouldn't be intelligible anyway).
//                    Lets the deck function as a tape player for music.
//
// On Apple II the cassette is always-on hardware (no plug/unplug toggle
// like POM1's ACI card), so the `aciActive` gate from POM1 is dropped:
// any loaded program tape feeds $C060 unconditionally.

#ifndef POM2_CASSETTE_DEVICE_H
#define POM2_CASSETTE_DEVICE_H

#include "AudioDevice.h"
#include "CpuClock.h"
#include "third_party/miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class CassetteDevice : public AudioSource, public RateAware
{
public:
    enum class DeckMode { NoTape, ProgramTape, AudioStream };
    DeckMode getDeckMode() const {
        if (!loadedTapeReady) return DeckMode::NoTape;
        return audioStreamMode ? DeckMode::AudioStream : DeckMode::ProgramTape;
    }

    CassetteDevice();
    ~CassetteDevice() override = default;

    /// Full reset of every cassette-side state (loaded tape preserved,
    /// recording and playback progress wiped). Called at construction only.
    void reset();
    /// Apple II hard-reset: clears the output flip-flop + cycle base.
    /// Loaded tape, playback position, recording buffer, paused flag and
    /// rewinding flag all survive — a real deck doesn't rewind itself just
    /// because the host got power-cycled.
    void resetCpuSide();
    void advanceCycles(int cycles);

    /// $C060 read: bit-7 = sign of the cassette input comparator.
    uint8_t readTapeInput();
    /// $C020 access (read or write): toggles the cassette OUTPUT line.
    /// Returns the new output level (bit-7) so the caller can use it as
    /// the bus byte if needed.
    uint8_t toggleOutput();

    bool loadTape(const std::string& path);
    bool saveTape(const std::string& path) const;

    void rewindTape();
    void playTape();
    void stopTape();
    void ejectTape();
    void clearRecordedTape();

    void setPlaybackPaused(bool paused);
    bool isPlaybackPaused() const { return playbackPaused.load(std::memory_order_relaxed); }

    /// "Leader auto-rewind" — POM2-only convenience that re-arms the
    /// tape at the start when the Monitor's READ routine hasn't polled
    /// `$C060` for ≥ 500 ms (assumes user was typing in BASIC and
    /// wants the next READ to see the leader fresh). MAME never
    /// rewinds — turn this OFF for custom loaders that poll the
    /// cassette input sporadically (Penguin Software fast loaders,
    /// some BASIC games with "loading…" prompts). Default OFF as of
    /// 2026-05-16. Persisted as `cassette_auto_rewind`.
    void setAutoRewind(bool enable) { autoRewindEnabled = enable; }
    bool isAutoRewindEnabled() const { return autoRewindEnabled; }

    /// Stream-mode only seek (no-op in pulse mode).
    void seekRelativeSeconds(double deltaSeconds);

    /// Stream-mode only: current cursor / total length in seconds.
    double getPlaybackPositionSeconds() const;
    double getPlaybackTotalSeconds() const;

    bool hasLoadedTape() const { return loadedTapeReady; }
    bool hasRecordedTape() const { return !recordedDurations.empty(); }
    bool isPlaybackActive() const { return playbackActive; }
    bool isPlaybackArmed()  const { return playbackArmed; }
    bool isRewinding()      const { return rewinding; }
    bool isAudioAvailable() const { return audioAvailable; }
    bool isAudioStreamMode() const { return audioStreamMode; }
    double getQueuedAudioSeconds() const;
    size_t getLoadedTransitionCount() const {
        return audioStreamMode ? static_cast<size_t>(audioStreamTotalFrames) : loadedDurations.size();
    }
    size_t getRecordedTransitionCount() const { return recordedDurations.size(); }
    const std::string& getLoadedTapePath() const { return loadedTapePath; }
    const std::string& getLoadInfo()       const { return loadInfo; }
    const std::string& getLastError()      const { return lastError; }

    /// AudioSource — generates cassette audio samples.
    void fillAudioBuffer(float* output, int frameCount) override;

    void setAudioAvailable(bool available) { audioAvailable = available; }
    void setAudioOutputSampleRate(uint32_t hz) { audioOutputSampleRate = std::max<uint32_t>(1, hz); }
    /// RateAware override — forwards to setAudioOutputSampleRate so
    /// AudioDevice::addSource auto-config Just Works.
    void setSampleRate(uint32_t hz) override { setAudioOutputSampleRate(hz); }

    void  setVolume(float v);
    float getVolume() const { return volume.load(std::memory_order_relaxed); }
    void  setMuted(bool m) { muted.store(m, std::memory_order_relaxed); }
    bool  isMuted() const  { return muted.load(std::memory_order_relaxed); }

    /// Arm recording without requiring a CPU $C020 toggle. The deck's REC
    /// button uses this so a scripted run can capture output that the
    /// program emits as soon as it reaches the cassette.
    void armRecording() { beginRecordingIfNeeded(); }

private:
    // The Apple II cassette (and its tape file format) is timed in CPU
    // cycles. Same constant as POM1: aligns 770 Hz sync exactly on the
    // emulated CPU clock so .wav/.mp3 round-trip without phase drift.
    static constexpr uint32_t kRealtimeAudioTimebaseHz =
        static_cast<uint32_t>(POM2_CPU_CLOCK_HZ);
    static constexpr uint32_t kTapeFileTimebaseHz =
        static_cast<uint32_t>(POM2_CPU_CLOCK_HZ);
    static constexpr uint32_t kWavFileSampleRate = 44100;

    void queueAudioSegment(uint32_t cycles, bool level);
    void stopPulseAudio();
    void advancePlayback(uint32_t cycles);
    void advanceRewind(uint32_t cycles);
    static constexpr uint32_t kRewSpeedFactor = 20;

    bool loadAciTape(const std::string& path);
    bool saveAciTape(const std::string& path) const;
    bool loadWavTape(const std::string& path);
    bool saveWavTape(const std::string& path) const;
    bool loadMiniaudioTape(const std::string& path);
    bool loadAudioStream(const std::string& path);
    void closeAudioStream();

    static bool pcmToDurations(const std::vector<float>& mono,
                               uint32_t sampleRate,
                               std::vector<uint32_t>& outDurations,
                               bool& outInitialLevel,
                               std::string& outErr);

    bool loadPlaybackDurations(std::vector<uint32_t> durations,
                               bool initialLevel,
                               const std::string& path);

    void resetPlaybackState();
    void beginRecordingIfNeeded();
    void clearLiveAudioState();
    void armPlaybackAtStart();

    static std::string lookupTapeInfo(const std::string& path);

private:
    bool audioAvailable = false;
    bool autoRewindEnabled = false;     // see setAutoRewind
    uint32_t audioOutputSampleRate = kWavFileSampleRate;

    struct AudioSegment {
        uint32_t remainingSamples;
        float    sampleValue;
    };

    mutable std::mutex audioMutex;
    std::deque<AudioSegment> audioQueue;
    float    audioPlaybackSample       = 0.0f;
    // Atomic: touched by the program-tape fill path (under audioMutex), the
    // stream fill path (under audioStreamMutex) and setPlaybackPaused — i.e.
    // across different locks, so a plain int would be a visibility race.
    std::atomic<uint32_t> audioRampInSamplesRemaining{0};

    // Mechanical "clunk" mixed on top of whatever is playing whenever the
    // deck mode flips (NoTape ↔ ProgramTape ↔ AudioStream).
    std::vector<float> clickBuffer;
    size_t   clickCursor   = 0;
    DeckMode lastDeckMode  = DeckMode::NoTape;

    void playMechanicalClick();
    void fireClickIfModeChanged();

    uint64_t currentCycle = 0;
    double   audioSampleRemainder = 0.0;

    bool     outputLevel             = false;
    bool     recordedInitialLevel    = false;
    uint64_t lastOutputToggleCycle   = 0;
    std::vector<uint32_t> recordedDurations;

    bool     inputLevel        = false;
    bool     loadedInitialLevel = false;
    bool     loadedTapeReady   = false;
    bool     playbackArmed     = false;
    uint64_t lastTapeInputCycle = 0;
    bool     playbackActive    = false;
    uint64_t cyclesUntilInputToggle = 0;
    size_t   playbackIndex     = 0;
    bool     rewinding         = false;
    uint64_t rewCarryCycles    = 0;
    std::vector<uint32_t> loadedDurations;
    std::string loadedTapePath;
    std::string loadInfo;

    std::atomic<float> volume{1.0f};
    std::atomic<bool>  muted{false};
    std::atomic<bool>  playbackPaused{false};

    // Stream mode: miniaudio decodes the file on demand at the device
    // output rate, no pulse extraction.
    mutable std::mutex audioStreamMutex;
    // Atomic: read lock-free on the audio-callback thread (fillAudioBuffer's
    // first branch, before any lock) while the UI thread flips it in
    // loadTape/ejectTape/loadAudioStream — a plain bool here is a data race.
    std::atomic<bool> audioStreamMode{false};
    bool      audioStreamDecoderOpen  = false;
    ma_decoder audioStreamDecoder{};
    uint64_t  audioStreamCursor       = 0;
    uint64_t  audioStreamTotalFrames  = 0;

    mutable std::string lastError;
};

#endif // POM2_CASSETTE_DEVICE_H

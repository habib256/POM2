// POM2 Apple II Emulator
// Copyright (C) 2026
//
// EchoPlusCard — see header for address map + protocol notes.

#include "EchoPlusCard.h"

#include "M6502.h"

#include <algorithm>
#include <vector>

// ─── AudioSrc — silent placeholder until phoneme PCM data is imported ──

struct EchoPlusCard::AudioSrc : public AudioSource, public RateAware
{
    explicit AudioSrc(EchoPlusCard* p) : parent(p) {}

    EchoPlusCard* parent;

    std::atomic<uint32_t> sampleRate { AudioDevice::kSampleRate };
    std::atomic<float>    volume     { 0.7f };
    std::atomic<bool>     muted      { false };

    void setSampleRate(uint32_t hz) override
    {
        if (hz == 0) hz = AudioDevice::kSampleRate;
        sampleRate.store(hz, std::memory_order_relaxed);
    }

    void fillAudioBuffer(float* output, int frameCount) override
    {
        if (frameCount <= 0) return;
        std::fill_n(output, frameCount, 0.0f);
        if (muted.load(std::memory_order_relaxed)) return;
        const float vol = volume.load(std::memory_order_relaxed);
        const uint32_t sr = sampleRate.load(std::memory_order_relaxed);
        // Render under the parent mutex so the chip's playback cursor
        // is consistent with CPU-thread register writes. Render into a
        // temp buffer + scale by master volume so the chip's own
        // amplitude register stays in `Ssi263::fillAudio` (single
        // source of truth for chip-side scaling).
        std::vector<float> tmp(static_cast<size_t>(frameCount), 0.0f);
        {
            std::lock_guard<std::mutex> lk(parent->mtx_);
            parent->ssi_.fillAudio(tmp.data(), frameCount, sr);
        }
        for (int i = 0; i < frameCount; ++i) output[i] = tmp[i] * vol;
    }
};

// ─── EchoPlusCard ─────────────────────────────────────────────────────────

EchoPlusCard::EchoPlusCard(int slotNum)
    : slot_(slotNum)
{
    audio_ = std::make_unique<AudioSrc>(this);
    onReset();
}

EchoPlusCard::~EchoPlusCard() = default;

void EchoPlusCard::onUnplug()
{
    // SlotBus auto-releases pending IRQ on detach.
}

void EchoPlusCard::onReset()
{
    std::lock_guard<std::mutex> lk(mtx_);
    ssi_.reset();
    assertIrq(false);
}

AudioSource* EchoPlusCard::audioSource() { return audio_.get(); }

void EchoPlusCard::setSampleRate(uint32_t hz)
{
    if (hz == 0) hz = AudioDevice::kSampleRate;
    audio_->sampleRate.store(hz, std::memory_order_relaxed);
}

void EchoPlusCard::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    audio_->volume.store(v, std::memory_order_relaxed);
}

float EchoPlusCard::getVolume() const
{
    return audio_->volume.load(std::memory_order_relaxed);
}

void EchoPlusCard::setMuted(bool m) { audio_->muted.store(m, std::memory_order_relaxed); }
bool EchoPlusCard::isMuted()  const { return audio_->muted.load(std::memory_order_relaxed); }

uint8_t EchoPlusCard::slotRomRead(uint8_t low8)
{
    std::lock_guard<std::mutex> lk(mtx_);
    // Only the first 5 bytes are decoded. Real Echo II returns open-bus
    // ($FF) for the rest of the slot ROM page.
    if (low8 > pom2::Ssi263::REG_FILFREQ) return 0xFF;
    return ssi_.read(low8);
}

void EchoPlusCard::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx_);
    if (low8 > pom2::Ssi263::REG_FILFREQ) return;
    const bool aRequestCleared = ssi_.write(low8, v);
    if (aRequestCleared) {
        // Host ack'd the previous phoneme request → drop slot IRQ.
        assertIrq(false);
    }
}

void EchoPlusCard::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    std::lock_guard<std::mutex> lk(mtx_);
    if (ssi_.advance(cycles)) {
        // Phoneme just finished + chip is requesting next → assert IRQ.
        assertIrq(true);
    }
}

void EchoPlusCard::updateIrqFromChip()
{
    // Called by external test code if needed; production path goes
    // through advanceCycles / slotRomWrite which already manage the
    // IRQ line transitions.
    assertIrq(ssi_.aRequest() && ssi_.irqEnabled());
}

EchoPlusCard::ChipSnap EchoPlusCard::snapshotChip() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    ChipSnap s{};
    for (int r = 0; r <= pom2::Ssi263::REG_FILFREQ; ++r) {
        s.regs[r] = ssi_.peekRegister(static_cast<uint8_t>(r));
    }
    s.currentPhoneme         = ssi_.currentPhoneme();
    s.mode                   = static_cast<uint8_t>(ssi_.currentMode());
    s.aRequest               = ssi_.aRequest();
    s.powerDown              = ssi_.powerDown();
    s.irqEnabled             = ssi_.irqEnabled();
    s.phonemeRemainingCycles = ssi_.phonemeRemainingCycles();
    s.phonemeWriteCount      = ssi_.phonemeWriteCount();
    return s;
}

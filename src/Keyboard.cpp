// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Keyboard.h"

namespace pom2 {

void Keyboard::queueKey(uint8_t apple2Key)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t value = apple2Key & 0x7F;
    if (!pasteQueue_.empty()) {
        pasteQueue_.push_back(value);
    } else {
        // Like the hardware latch, the newest unread live key wins.
        lastKey_ = value;
        ready_ = true;
    }
}

void Keyboard::clearStrobe()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = false;
    if (!pasteQueue_.empty()) {
        lastKey_ = pasteQueue_.front() & 0x7F;
        ready_ = true;
        pasteQueue_.pop_front();
    }
}

size_t Keyboard::pasteText(const char* data, size_t length,
                           bool supportsLowercase)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);

    const size_t inFlight = pasteQueue_.size() + (ready_ ? 1u : 0u);
    const size_t room = inFlight >= kPasteMaxChars
        ? 0u : kPasteMaxChars - inFlight;

    size_t queued = 0;
    bool previousWasCr = false;
    for (size_t i = 0; i < length && queued < room; ++i) {
        uint8_t value = static_cast<uint8_t>(data[i]);
        if (value == '\r') {
            value = 0x0D;
            previousWasCr = true;
        } else if (value == '\n') {
            if (previousWasCr) {
                previousWasCr = false;
                continue;
            }
            value = 0x0D;
            previousWasCr = false;
        } else {
            previousWasCr = false;
        }

        if (value < 0x20 && value != 0x0D && value != 0x09) continue;
        value &= 0x7F;
        if (!supportsLowercase && value >= 'a' && value <= 'z')
            value = static_cast<uint8_t>(value - 'a' + 'A');

        if (!ready_ && pasteQueue_.empty()) {
            lastKey_ = value;
            ready_ = true;
        } else {
            pasteQueue_.push_back(value);
        }
        ++queued;
    }
    return queued;
}

size_t Keyboard::pasteRawKeys(const char* data, size_t length)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t inFlight = pasteQueue_.size() + (ready_ ? 1u : 0u);
    const size_t room = inFlight >= kPasteMaxChars
        ? 0u : kPasteMaxChars - inFlight;

    size_t queued = 0;
    for (size_t i = 0; i < length && queued < room; ++i) {
        const uint8_t value = static_cast<uint8_t>(data[i]) & 0x7F;
        if (!ready_ && pasteQueue_.empty()) {
            lastKey_ = value;
            ready_ = true;
        } else {
            pasteQueue_.push_back(value);
        }
        ++queued;
    }
    return queued;
}

size_t Keyboard::pendingPasteSize() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return pasteQueue_.size();
}

void Keyboard::cancelPaste()
{
    std::lock_guard<std::mutex> lock(mutex_);
    pasteQueue_.clear();
}

void Keyboard::reset()
{
    std::lock_guard<std::mutex> lock(mutex_);
    ready_ = false;
    pasteQueue_.clear();
}

uint8_t Keyboard::latch() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint8_t>(lastKey_ | (ready_ ? 0x80 : 0x00));
}

uint8_t Keyboard::latchedCharacter() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint8_t>(lastKey_ & 0x7F);
}

} // namespace pom2

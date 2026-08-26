// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Apple II keyboard latch plus host paste FIFO. The memory bus owns $C0xx
// decode; this class owns the thread-safe input state and strobe behaviour.

#ifndef POM2_KEYBOARD_H
#define POM2_KEYBOARD_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace pom2 {

class Keyboard
{
public:
    static constexpr size_t kPasteMaxChars = 4096;

    void queueKey(uint8_t apple2Key);
    void clearStrobe();

    size_t pasteText(const char* data, size_t length, bool supportsLowercase);
    size_t pasteText(const std::string& text, bool supportsLowercase)
    { return pasteText(text.data(), text.size(), supportsLowercase); }
    size_t pasteRawKeys(const char* data, size_t length);

    size_t pendingPasteSize() const;
    void cancelPaste();
    void reset();

    /// Character in bits 0..6 and keyboard strobe in bit 7.
    uint8_t latch() const;
    uint8_t latchedCharacter() const;

private:
    mutable std::mutex mutex_;
    uint8_t lastKey_ = 0;
    bool ready_ = false;
    std::deque<uint8_t> pasteQueue_;
};

} // namespace pom2

#endif // POM2_KEYBOARD_H

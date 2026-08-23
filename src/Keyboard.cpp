// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Keyboard — see Keyboard.h. Bodies moved verbatim from Memory.cpp (the paste
// FIFO's control-byte filtering, CR/LF collapse and case-fold, and the
// newest-wins latch), with only the member renames Memory used inline
// (lastKey → lastKey_, keyReady → keyReady_, pasteQueue → pasteQueue_,
// publishKbLatch → publish, kbMutex → mtx_).

#include "Keyboard.h"

namespace pom2 {

void Keyboard::queueKey(uint8_t apple2Key)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const uint8_t b = apple2Key & 0x7F;
    if (!pasteQueue_.empty()) {
        // A host paste is draining — append so this live keystroke is
        // delivered in order AFTER it, instead of clobbering the currently
        // latched paste byte and jumping the FIFO.
        pasteQueue_.push_back(b);
    } else {
        // No paste in flight: behave like the hardware latch — newest key
        // wins (fast typing overwrites an unread key, as on real hardware).
        lastKey_  = b;
        keyReady_ = true;
        publish();
    }
}

void Keyboard::clearStrobe()
{
    std::lock_guard<std::mutex> lk(mtx_);
    // Apple II hardware leaves the key byte in the latch and only releases
    // the strobe — KEYIN re-polls $C000 until a fresh key arrives.
    keyReady_ = false;
    // Paste-queue drain: if the user has a paste in flight, the moment
    // the strobe is cleared we promote the next byte into the latch and
    // re-arm the strobe. The CPU's $C000-poll loop will see the next char
    // on its very next iteration — no timing tricks, the ROM clocks the
    // paste out at exactly the rate it can consume.
    if (!pasteQueue_.empty()) {
        lastKey_  = pasteQueue_.front() & 0x7F;
        keyReady_ = true;
        pasteQueue_.pop_front();
    }
    publish();
}

std::size_t Keyboard::pasteText(const char* data, std::size_t length, bool foldToUpper)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(mtx_);

    // Cap against the LIVE queue size, not just this call, so repeated pastes
    // can't grow pasteQueue_ without bound (a memory DoS via the AI-control or
    // clipboard paths).
    const std::size_t inFlight = pasteQueue_.size() + (keyReady_ ? 1u : 0u);
    const std::size_t room = (inFlight >= kPasteMaxChars) ? 0u : (kPasteMaxChars - inFlight);

    std::size_t queued = 0;
    bool   prevWasCR = false;
    for (std::size_t i = 0; i < length && queued < room; ++i) {
        uint8_t b = static_cast<uint8_t>(data[i]);

        // Line-ending normalisation: \r, \n, and \r\n all collapse to one
        // CR ($0D). Track the previous byte so the LF after CR doesn't
        // produce a second CR.
        if (b == '\r') {
            b = 0x0D;
            prevWasCR = true;
        } else if (b == '\n') {
            if (prevWasCR) { prevWasCR = false; continue; }  // swallowed
            b = 0x0D;
            prevWasCR = false;
        } else {
            prevWasCR = false;
        }

        // Drop unprintable controls except CR and HT. Apple II keyboard
        // ROM doesn't emit anything below $20 outside those two anyway.
        if (b < 0x20 && b != 0x0D && b != 0x09) continue;
        // Strip the high bit — Apple II is 7-bit ASCII.
        b &= 0x7F;
        // The ][ / ][+ keyboard has no lowercase; fold a-z → A-Z so pasted
        // BASIC/Monitor input is accepted (a real II keyboard can't emit
        // $61-$7A). IIe-class keyboards do have lowercase, so leave them —
        // the caller passes foldToUpper = !iieMode.
        if (foldToUpper && b >= 'a' && b <= 'z') b = static_cast<uint8_t>(b - 'a' + 'A');

        // First byte goes straight into the latch if it's empty; rest go
        // into the queue and drain via clearStrobe().
        if (!keyReady_ && pasteQueue_.empty()) {
            lastKey_  = b;
            keyReady_ = true;
            publish();
        } else {
            pasteQueue_.push_back(b);
        }
        ++queued;
    }
    return queued;
}

std::size_t Keyboard::pasteRawKeys(const char* data, std::size_t length)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(mtx_);
    const std::size_t inFlight = pasteQueue_.size() + (keyReady_ ? 1u : 0u);
    const std::size_t room = (inFlight >= kPasteMaxChars) ? 0u : (kPasteMaxChars - inFlight);
    std::size_t queued = 0;
    for (std::size_t i = 0; i < length && queued < room; ++i) {
        const uint8_t b = static_cast<uint8_t>(data[i]) & 0x7F;
        if (!keyReady_ && pasteQueue_.empty()) {
            lastKey_  = b;
            keyReady_ = true;
            publish();
        } else {
            pasteQueue_.push_back(b);
        }
        ++queued;
    }
    return queued;
}

} // namespace pom2

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Apple II / II+ memory: 48 KB RAM ($0000-$BFFF), I/O page ($C000-$C0FF),
// slot ROM area ($C100-$C7FF, currently empty), 12 KB Monitor + Applesoft
// ROM ($D000-$FFFF). Soft switches at $C050-$C057 drive display modes.

#ifndef POM2_MEMORY_H
#define POM2_MEMORY_H

#include "SlotBus.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

class CassetteDevice;
class M6502;
class SpeakerDevice;

class Memory
{
public:
    Memory();

    // Built-in cassette interface — Apple II routes $C020 (output toggle)
    // and $C060 (input bit-7) directly to the cassette without a card.
    // Pointer is non-owning; lifetime managed by EmulationController.
    void setCassetteDevice(CassetteDevice* dev) { cassette = dev; }

    /// Built-in speaker (1-bit flip-flop at $C030-$C03F). Non-owning
    /// pointer set by EmulationController. The CPU pointer is needed
    /// by the $C030 handler to timestamp toggles with sub-instruction
    /// precision (`cycleCounter + cpu->getCurrentInstructionCycles()`).
    void setSpeakerDevice(SpeakerDevice* s) { speaker = s; }
    void setCpu(M6502* c)                   { cpu = c; }

    /// Apple II expansion bus — slots 0-7. Cards plug directly via the
    /// SlotBus. Memory routes $C080-$CFFF accesses through it.
    SlotBus&       slotBus()       { return slots; }
    const SlotBus& slotBus() const { return slots; }

    /// Flat 64 KB RAM mode — bypasses soft switches, slot bus and ROM
    /// write protection. Every access is plain mem[addr]. Used **only**
    /// by the Klaus Dormann functional test, which expects the whole
    /// address space to behave as RAM. Must NOT be enabled in normal
    /// emulation; no safety checks remain.
    void setTestMode(bool enabled) { testMode = enabled; }
    bool isTestMode() const        { return testMode; }

    // CPU bus interface (called from M6502).
    uint8_t memRead(uint16_t addr);
    void    memWrite(uint16_t addr, uint8_t value);

    // Diagnostic — used by M6502's BRK trace. Cheap stub for now.
    std::string busStateSummary() const { return " (no extra cards)"; }

    // ROM loading. Apple II distributions ship as a single 12 KB image
    // covering $D000-$FFFF. Returns 1 on success, 0 on failure (last
    // error in `lastError`).
    int loadAppleIIRom(const char* filename);
    int loadCharRom(const char* filename);  // 2 KB, 256 glyphs × 8 rows

    const std::string& getLastError() const { return lastError; }

    // Direct access for the display / debugger / snapshot.
    const uint8_t* data() const { return mem.data(); }
    uint8_t* dataMutable()      { return mem.data(); }

    /// Bulk ROM load — bypasses the writable[] bitmap. Used by RomLoader
    /// (and by future cards) to flash their ROM image into protected
    /// regions without having to flip ROM-protect manually. `addr +
    /// length` must be ≤ $10000.
    bool loadRomBytes(const uint8_t* src, size_t length, uint16_t addr);

    /// Mark `[lo, hi]` as ROM (writes silently dropped). Public so cards
    /// can declare their ROM windows after loading. Idempotent.
    void markRomRange(uint16_t lo, uint16_t hi) { markRomRegion(lo, hi); }

    // Character ROM access (2 KB, 8 bytes/glyph). May be empty if the
    // user hasn't loaded a charset — we then fall back to a built-in
    // ASCII table at render time.
    const std::vector<uint8_t>& charRom() const { return characterRom; }

    // Soft-switch state (read by the display / speaker / paddle code).
    struct DisplayState {
        bool textMode  = true;   // $C050 clear, $C051 set
        bool mixedMode = false;  // $C052 clear, $C053 set
        bool page2     = false;  // $C054 page 1, $C055 page 2
        bool hiRes     = false;  // $C056 lo-res, $C057 hi-res
    };
    DisplayState getDisplayState() const {
        std::lock_guard<std::mutex> lk(stateMutex);
        return display;
    }

    // Keyboard bridge — UI thread enqueues keys, CPU thread reads them
    // via $C000 / clears the strobe via $C010.
    void queueKey(uint8_t ascii);       // 7-bit ASCII (upper or lower case)
    void clearKeyStrobe();              // mirrors $C010 access

    /// Paste a block of text. Line-endings normalised to CR ($0D) — `\r\n`,
    /// `\r`, `\n` all collapse to one CR. Non-printable controls below
    /// `$20` other than CR / HT are dropped silently. Capped at
    /// `kPasteMaxChars` so a runaway clipboard can't overwhelm the queue.
    /// Bytes are appended to an internal FIFO that drains one byte per
    /// `clearKeyStrobe()` — so the Apple II ROM's strobe-and-poll loop
    /// pulls them out at exactly its own pace, no timing tricks needed.
    /// Returns the number of bytes actually queued (after filtering).
    static constexpr size_t kPasteMaxChars = 4096;
    size_t pasteText(const char* data, size_t length);
    size_t pasteText(const std::string& s) { return pasteText(s.data(), s.size()); }

    /// How many bytes are still waiting in the paste queue. UI shows this
    /// in the Emulation panel + Edit menu so the user knows the paste is
    /// in flight.
    size_t pendingPasteSize() const;
    void   cancelPaste();

    // Speaker — the CPU toggles a flip-flop by reading $C030. We expose
    // a counter the audio backend can sample. UI may also subscribe via
    // setSpeakerToggleCallback().
    uint64_t getSpeakerToggleCount() const { return speakerToggles.load(); }
    using SpeakerCallback = void (*)(void* user);
    void setSpeakerCallback(SpeakerCallback cb, void* user) {
        speakerCb = cb; speakerUser = user;
    }

    // Paddle inputs — $C064-$C067 read the 4 paddles, $C061-$C063 read
    // the 3 push-button switches. Game-port reset latch armed by $C070.
    void setPaddle(int idx, uint8_t value);    // 0..3
    void setPaddleButton(int idx, bool down);  // 0..2

    // Reset — clears the keyboard strobe, returns to text/page 1 mode.
    // Does NOT touch RAM (matches Apple II reset behaviour: RESET only
    // jumps through ($FFFC) without zeroing memory).
    void resetSoftSwitches();

    // Power-cycle helper: wipe user RAM ($0000-$BFFF). Leaves the I/O page,
    // slot ROM area, and main ROM ($D000-$FFFF) intact.
    void clearRam();

    // CPU pacing hook — EmulationController calls this with the cycle
    // count returned by M6502::run() so the paddle RC discharge timer
    // ticks against the real CPU clock instead of wallclock. Forwards to
    // the cassette device too (so its pulse advance stays cycle-aligned).
    void advanceCycles(int cycles);
    uint64_t getCycleCounter() const { return cycleCounter; }

private:
    std::array<uint8_t, 0x10000> mem{};       // flat 64 KB RAM/ROM mirror
    std::array<bool,    0x10000> writable{};  // false in ROM regions
    std::vector<uint8_t> characterRom;        // 2048 bytes once loaded
    std::string lastError;

    // Soft-switch state, guarded by stateMutex. Reads from the UI thread
    // are infrequent (once per frame, in render()).
    mutable std::mutex stateMutex;
    DisplayState display;

    // Keyboard.
    mutable std::mutex kbMutex;
    uint8_t lastKey   = 0;
    bool    keyReady  = false;
    // Paste queue — drains one byte into `lastKey` each time the CPU
    // clears the strobe via $C010. See pasteText().
    std::deque<uint8_t> pasteQueue;

    // Speaker.
    std::atomic<uint64_t> speakerToggles{0};
    SpeakerCallback speakerCb = nullptr;
    void*           speakerUser = nullptr;

    // Paddles + buttons. Paddle "value" is the discharge time of an RC
    // network — Apple II reads $C064-$C067 and counts cycles until the
    // register flips. We model that by remembering when the latch was armed.
    std::array<uint8_t, 4> paddleValue{ 128, 128, 128, 128 };
    std::array<bool,    3> paddleButton{ false, false, false };
    uint64_t paddleLatchCycle = 0;
    uint64_t cycleCounter     = 0;       // hand-rolled, see advanceCycles()

    // Cassette: non-owning pointer set by EmulationController.
    CassetteDevice* cassette = nullptr;

    // Speaker + CPU back-pointers for $C030 sub-instruction timestamping.
    SpeakerDevice* speaker = nullptr;
    M6502*         cpu     = nullptr;

    // Expansion bus — owns plugged cards.
    SlotBus slots;

    // Klaus harness flat-RAM mode. See setTestMode().
    bool testMode = false;

    void markRomRegion(uint16_t lo, uint16_t hi);
    uint8_t softSwitchAccess(uint16_t addr, bool isWrite, uint8_t writeVal);
};

#endif // POM2_MEMORY_H

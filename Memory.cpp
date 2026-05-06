// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Memory.h"
#include "CassetteDevice.h"
#include "Logger.h"
#include "M6502.h"
#include "SpeakerDevice.h"

#include <cstdio>
#include <cstring>
#include <fstream>

Memory::Memory()
{
    mem.fill(0);
    writable.fill(true);
    lcBank1.fill(0);
    lcBank2.fill(0);
    lcHigh.fill(0);
    // ROM region. The Apple II //e bank-switched language card is NOT
    // modelled — $D000-$FFFF is plain ROM. The slot ROM range
    // $C100-$C7FF is NOT marked as ROM here: SlotBus owns that window
    // and dispatches reads to plugged cards (writes are dropped inside
    // Memory::memWrite).
    markRomRegion(0xD000, 0xFFFF);  // Applesoft + Monitor

    // Default reset vector points at $F800 (Monitor cold start) so a
    // fresh boot without ROM loaded still runs *something* (BRK loop)
    // instead of jumping into uninitialised memory.
    mem[0xFFFC] = 0x00;
    mem[0xFFFD] = 0xF8;
    // IRQ/BRK and NMI both land at $F800 by default.
    mem[0xFFFE] = 0x00;
    mem[0xFFFF] = 0xF8;
    mem[0xFFFA] = 0x00;
    mem[0xFFFB] = 0xF8;
}

std::string Memory::busStateSummary() const
{
    if (!lcReadRam && !lcWriteEnable) return " (LC: ROM)";
    std::string s = " (LC: ";
    s += lcReadRam ? "RAM" : "ROM";
    s += lcBank2Active ? " bank2" : " bank1";
    s += lcWriteEnable ? " writable)" : " write-protected)";
    return s;
}

void Memory::markRomRegion(uint16_t lo, uint16_t hi)
{
    for (int a = lo; a <= hi; ++a) writable[a] = false;
}

bool Memory::loadRomBytes(const uint8_t* src, size_t length, uint16_t addr)
{
    if (!src || length == 0) return false;
    if (static_cast<size_t>(addr) + length > 0x10000) return false;
    std::memcpy(mem.data() + addr, src, length);
    return true;
}

int Memory::loadAppleIIRom(const char* filename)
{
    std::ifstream f(filename, std::ios::binary);
    if (!f) {
        lastError = std::string("Cannot open ROM: ") + filename;
        pom2::log().warn("ROM", lastError);
        return 0;
    }
    f.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);

    // Common image sizes:
    //   12 KB ($D000-$FFFF) = Apple II+ Autostart + Applesoft
    //   16 KB ($C000-$FFFF) = Apple //e (we drop the I/O page so soft
    //                          switches keep working)
    uint16_t loadAddr = 0;
    if (size == 12 * 1024) {
        loadAddr = 0xD000;
    } else if (size == 16 * 1024) {
        loadAddr = 0xC000;
    } else if (size >= 0x800 && size <= 0x10000) {
        // Best effort: fit at the high end so vectors land at $FFFA-$FFFF.
        loadAddr = static_cast<uint16_t>(0x10000 - size);
    } else {
        lastError = "Unexpected ROM size: " + std::to_string(size);
        pom2::log().warn("ROM", lastError);
        return 0;
    }

    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), size);
    if (!f) {
        lastError = "Short read";
        return 0;
    }
    // Don't overwrite the I/O page when loading a 16 KB image.
    for (size_t i = 0; i < size; ++i) {
        uint16_t addr = static_cast<uint16_t>(loadAddr + i);
        if (addr >= 0xC000 && addr <= 0xC0FF) continue;
        mem[addr] = buf[i];
    }
    pom2::log().info("ROM", std::string("Loaded ") + filename + " at $" +
                     [&]{ char b[8]; std::snprintf(b, 8, "%04X", loadAddr); return std::string(b); }());
    return 1;
}

int Memory::loadCharRom(const char* filename)
{
    std::ifstream f(filename, std::ios::binary);
    if (!f) {
        lastError = std::string("Cannot open char ROM: ") + filename;
        pom2::log().warn("ROM", lastError);
        return 0;
    }
    f.seekg(0, std::ios::end);
    auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != 2048 && size != 4096 && size != 8192) {
        lastError = "Char ROM must be 2K/4K/8K, got " + std::to_string(size);
        return 0;
    }
    characterRom.resize(size);
    f.read(reinterpret_cast<char*>(characterRom.data()), size);
    return f ? 1 : 0;
}

void Memory::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    cycleCounter += cycles;
    if (cassette) cassette->advanceCycles(cycles);
    slots.advanceCycles(cycles);
}

void Memory::resetSoftSwitches()
{
    std::lock_guard<std::mutex> lk(stateMutex);
    display = DisplayState{};
    lcReadRam     = false;
    lcWriteEnable = false;
    lcBank2Active = true;
    lcPrewrite    = false;
    std::lock_guard<std::mutex> kb(kbMutex);
    keyReady = false;
}

void Memory::clearRam()
{
    // Wipe user RAM only. The Language Card is RAM too, so a power-cycle
    // clears it even though its address window overlaps motherboard ROM.
    std::fill(mem.begin(), mem.begin() + 0xC000, 0);
    lcBank1.fill(0);
    lcBank2.fill(0);
    lcHigh.fill(0);
}

void Memory::queueKey(uint8_t apple2Key)
{
    std::lock_guard<std::mutex> lk(kbMutex);
    lastKey  = apple2Key & 0x7F;
    keyReady = true;
}

void Memory::clearKeyStrobe()
{
    std::lock_guard<std::mutex> lk(kbMutex);
    // Apple II hardware leaves the key byte in the latch and only releases
    // the strobe — KEYIN re-polls $C000 until a fresh key arrives.
    keyReady = false;
    // Paste-queue drain: if the user has a paste in flight, the moment
    // the strobe is cleared we promote the next byte into the latch and
    // re-arm the strobe. The CPU's $C000-poll loop will see the next char
    // on its very next iteration — no timing tricks, the ROM clocks the
    // paste out at exactly the rate it can consume.
    if (!pasteQueue.empty()) {
        lastKey  = pasteQueue.front() & 0x7F;
        keyReady = true;
        pasteQueue.pop_front();
    }
}

size_t Memory::pasteText(const char* data, size_t length)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(kbMutex);

    size_t queued = 0;
    bool   prevWasCR = false;
    for (size_t i = 0; i < length && queued < kPasteMaxChars; ++i) {
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

        // First byte goes straight into the latch if it's empty; rest go
        // into the queue and drain via clearKeyStrobe().
        if (!keyReady && pasteQueue.empty()) {
            lastKey  = b;
            keyReady = true;
        } else {
            pasteQueue.push_back(b);
        }
        ++queued;
    }
    return queued;
}

size_t Memory::pendingPasteSize() const
{
    std::lock_guard<std::mutex> lk(kbMutex);
    return pasteQueue.size();
}

void Memory::cancelPaste()
{
    std::lock_guard<std::mutex> lk(kbMutex);
    pasteQueue.clear();
}

void Memory::setPaddle(int idx, uint8_t value)
{
    if (idx >= 0 && idx < 4) paddleValue[idx] = value;
}

void Memory::setPaddleButton(int idx, bool down)
{
    if (idx >= 0 && idx < 3) paddleButton[idx] = down;
}

uint8_t Memory::softSwitchAccess(uint16_t addr, bool /*isWrite*/, uint8_t /*writeVal*/)
{
    // Soft-switch byte is in $C000-$C07F. Many switches respond to either
    // a read OR a write (both edges work as toggles). We snapshot the
    // current keyboard latch under kbMutex on every $C000/$C010 access.
    uint8_t kbLatch = 0;
    {
        std::lock_guard<std::mutex> lk(kbMutex);
        kbLatch = lastKey | (keyReady ? 0x80 : 0x00);
    }

    const uint8_t low = static_cast<uint8_t>(addr & 0xFF);

    // Keyboard data — same byte regardless of read/write.
    if (low == 0x00) return kbLatch;
    // Keyboard strobe clear — read or write.
    if (low == 0x10) {
        clearKeyStrobe();
        return kbLatch & 0x7F;
    }
    // Language Card status mirrors (Apple IIe-compatible; harmless on II+).
    if (low == 0x11) return lcBank2Active ? 0x80 : 0x00;  // RDLCBNK2
    if (low == 0x12) return lcReadRam     ? 0x80 : 0x00;  // RDLCRAM

    // Display soft switches.
    if (low >= 0x50 && low <= 0x57) {
        std::lock_guard<std::mutex> lk(stateMutex);
        switch (low) {
            case 0x50: display.textMode  = false; break;
            case 0x51: display.textMode  = true;  break;
            case 0x52: display.mixedMode = false; break;
            case 0x53: display.mixedMode = true;  break;
            case 0x54: display.page2     = false; break;
            case 0x55: display.page2     = true;  break;
            case 0x56: display.hiRes     = false; break;
            case 0x57: display.hiRes     = true;  break;
        }
        return 0;
    }

    // 80COL ($C00C off / $C00D on). Apple II/II+ doesn't natively use this
    // (it's a //e switch) but Le Chat Mauve / Video-7 RGB cards co-opt it
    // as the data line of their 2-bit FIFO mode register, clocked by AN3.
    if (low == 0x0C || low == 0x0D) {
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            display.eightyCol = (low == 0x0D);
        }
        slots.broadcastVideoSwitch(addr);
        return 0;
    }

    // AN3 annunciator ($C05E off / $C05F on). Used as the FIFO clock by
    // Le Chat Mauve — every $C05E→$C05F rising edge pushes the current
    // 80COL bit into the card's mode register.
    if (low == 0x5E || low == 0x5F) {
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            display.an3 = (low == 0x5F);
        }
        slots.broadcastVideoSwitch(addr);
        return 0;
    }

    // Speaker click — toggles on every access ($C030-$C03F all alias).
    // The audio path uses cycleCounter + the CPU's current-instruction
    // cycle progress for sub-instruction timestamps; without this every
    // toggle inside a frame collapses to one cycle and the audio aliases.
    if (low >= 0x30 && low <= 0x3F) {
        speakerToggles.fetch_add(1, std::memory_order_relaxed);
        if (speakerCb) speakerCb(speakerUser);
        if (speaker) {
            const uint64_t now = cycleCounter +
                (cpu ? static_cast<uint64_t>(cpu->getCurrentInstructionCycles()) : 0);
            speaker->recordToggle(now);
        }
        return 0;
    }

    // Cassette OUTPUT toggle ($C020-$C02F): any access flips the
    // cassette write line. The Monitor's WRITE routine ($FECD) loops on
    // BIT $C020 to drive the magnetic head with 770 Hz / 1 kHz / 2 kHz
    // square waves encoding sync, ones and zeroes.
    if (low >= 0x20 && low <= 0x2F) {
        if (cassette) cassette->toggleOutput();
        return 0;
    }

    // Cassette INPUT ($C060 only): bit-7 = sign of the audio comparator.
    // The Monitor's READ routine ($FEFD) tight-loops on $C060 measuring
    // sign-flip durations to recover bits from the tape. Note: $C061-$C067
    // are NOT cassette aliases on the II/II+ — they're the paddle buttons
    // and paddle inputs, handled below.
    if (low == 0x60) {
        if (cassette) return cassette->readTapeInput();
        return 0;
    }

    // Push-buttons ($C061-$C063): negative when pressed.
    if (low == 0x61) return paddleButton[0] ? 0x80 : 0x00;
    if (low == 0x62) return paddleButton[1] ? 0x80 : 0x00;
    if (low == 0x63) return paddleButton[2] ? 0x80 : 0x00;

    // Paddle inputs ($C064-$C067): the register stays "negative" while the
    // RC network discharges, then drops. We approximate by holding the
    // negative state for `paddleValue` CPU cycles after the last $C070
    // strobe.
    if (low >= 0x64 && low <= 0x67) {
        const int idx = low - 0x64;
        const uint64_t elapsed = cycleCounter - paddleLatchCycle;
        // Apple II paddle: ~11 cycles per paddle-value step (max ~2816c).
        const uint64_t threshold = static_cast<uint64_t>(paddleValue[idx]) * 11;
        return (elapsed < threshold) ? 0x80 : 0x00;
    }

    // Paddle-trigger reset ($C070): arms the RC network.
    if (low == 0x70) {
        paddleLatchCycle = cycleCounter;
        return 0;
    }

    // Unknown soft switch — Apple II hardware floats the bus. Returning 0
    // is good enough for everything we ship.
    return 0;
}

uint8_t Memory::languageCardSwitchAccess(uint16_t addr)
{
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);

    // $C080-$C087 select bank 2, $C088-$C08F select bank 1. Within each
    // half, the low two bits choose ROM/RAM read mode and whether the
    // prewrite latch is armed. $C084-$C087 mirror $C080-$C083.
    const bool bank2 = (low4 & 0x08) == 0;
    const uint8_t mode = low4 & 0x03;
    const bool readRam = (mode == 0x00 || mode == 0x03);
    const bool writeCandidate = (mode == 0x01 || mode == 0x03);
    const bool previousPrewrite = lcPrewrite;

    lcBank2Active = bank2;
    lcReadRam = readRam;
    lcWriteEnable = writeCandidate && previousPrewrite;
    lcPrewrite = writeCandidate;

    return 0;
}

uint8_t Memory::languageCardRead(uint16_t addr) const
{
    if (!lcReadRam) return mem[addr];
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        return lcBank2Active ? lcBank2[off] : lcBank1[off];
    }
    return lcHigh[addr - 0xE000];
}

void Memory::languageCardWrite(uint16_t addr, uint8_t value)
{
    if (!lcWriteEnable) return;
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (lcBank2Active) lcBank2[off] = value;
        else               lcBank1[off] = value;
        return;
    }
    lcHigh[addr - 0xE000] = value;
}

uint8_t Memory::memRead(uint16_t addr)
{
    // Klaus harness: flat 64 KB RAM, no side effects.
    if (testMode) return mem[addr];

    // Fast path: RAM below $C000. Main ROM may be replaced by Language Card
    // RAM depending on the $C080-$C08F latch state.
    if (addr < 0xC000) return mem[addr];
    if (addr >= 0xD000) return languageCardRead(addr);

    // $C000-$C07F — built-in I/O page (keyboard, speaker, cassette,
    // display soft switches, paddles).
    if (addr <= 0xC07F) return softSwitchAccess(addr, /*isWrite=*/false, 0);

    // $C080-$C0FF — slot device-select (16 bytes per slot, slot N at
    // $C080+N*16; slot 0 = language card, slots 1-7 = expansion cards).
    if (addr <= 0xC08F) return languageCardSwitchAccess(addr);
    if (addr <= 0xC0FF) return slots.deviceSelectRead(addr);

    // $C100-$C7FF — slot ROM, 256 bytes per slot 1-7. Reading any byte
    // in this window also marks that slot as the active expansion-ROM
    // owner for $C800-$CFFF below.
    if (addr <= 0xC7FF) return slots.slotRomRead(addr);

    // $C800-$CFFF — shared 2 KB expansion ROM, owned by whichever slot
    // was most recently selected through $C100-$C7FF. Read or write to
    // $CFFF deactivates the active slot until the next slot-ROM access.
    return slots.expansionRomRead(addr);
}

void Memory::memWrite(uint16_t addr, uint8_t value)
{
    // Klaus harness: flat 64 KB RAM, no side effects.
    if (testMode) { mem[addr] = value; return; }

    // Fast path: writable RAM and Language Card overlay for $D000-$FFFF.
    if (addr < 0xC000) {
        if (!writable[addr]) return;
        mem[addr] = value;
        return;
    }
    if (addr >= 0xD000) {
        languageCardWrite(addr, value);
        return;
    }

    if (addr <= 0xC07F) {
        softSwitchAccess(addr, /*isWrite=*/true, value);
        return;
    }
    if (addr <= 0xC0FF) {
        if (addr <= 0xC08F) {
            languageCardSwitchAccess(addr);
            return;
        }
        slots.deviceSelectWrite(addr, value);
        return;
    }
    if (addr <= 0xC7FF) {
        // Slot ROM is read-only on real hardware. Drop the write but
        // still let the bus mark the slot as active so a write-into-rom
        // strobe (uncommon but legal) selects the slot for $C800-$CFFF.
        slots.slotRomRead(addr);
        return;
    }
    // $C800-$CFFF — expansion ROM, conventionally read-only. Forward
    // through SlotBus so $CFFF disable still works on writes (the
    // address bus is what matters, not the direction).
    slots.expansionRomWrite(addr, value);
}

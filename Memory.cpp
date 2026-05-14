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

void Memory::setCpu(M6502* c)
{
    cpu = c;
    // Re-install the SlotBus IRQ router whenever the CPU is rewired. The
    // closure captures the CPU pointer by value so a later swap doesn't
    // dangle — re-issuing setCpu() re-installs against the new pointer.
    // An empty function on `c == nullptr` disconnects stray assertIrq()
    // calls from cards that haven't been unplugged yet.
    if (c) {
        slots.setIrqRouter([c](int slot, bool asserted) {
            c->setIrqLine(slot, asserted);
        });
    } else {
        slots.setIrqRouter({});
    }
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

int Memory::loadAppleIIRom(const char* filename, bool pickLower16KFor32K)
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
    //   16 KB ($C000-$FFFF) = Apple //e — split into the internal I/O ROM
    //     ($C100-$CFFF, motherboard firmware) and the main ROM ($D000-$FFFF)
    //     when iieMode is on. On II+, the same 16 KB image just loads at
    //     $C000 and skips the I/O page (legacy behaviour).
    //   32 KB = TWO common layouts, indistinguishable by size alone:
    //     * Apple //e "system + video" combined dump. The 16 KB firmware
    //       lives at file offsets 0x4000-0x7FFF (reset vector at 0x7FFC
    //       maps to $FFFC); lower 16 KB carries video / char data we
    //       don't load through this path. → pickLower16KFor32K = false.
    //     * Apple //c / //c+ two-bank firmware dump. Bank 0 (cold-reset
    //       entry) at file offsets 0x0000-0x3FFF; bank 1 (alt firmware,
    //       AppleTalk / MouseText / SmartPort drivers) at 0x4000-0x7FFF.
    //       The two banks are swapped at runtime via the $C028 ROMBANK
    //       soft switch; bank 0 must be the one mapped at reset.
    //       → pickLower16KFor32K = true.
    uint16_t loadAddr = 0;
    bool iieFromUpper16K = false;
    size_t  skipBytes = 0;  // leading bytes to drop from the file
    if (size == 12 * 1024) {
        loadAddr = 0xD000;
    } else if (size == 16 * 1024) {
        loadAddr = 0xC000;
    } else if (size == 20 * 1024) {
        // Some Apple II+ dumps (notably the MAME "apple2_plus" combined
        // pack) prepend 4 KB of filler — typically zeros or an unused
        // alternate Integer BASIC bank — to the real 16 KB
        // $C000-$FFFF firmware. The high 16 KB is what every Apple II+
        // expects at $C000 onwards. Skip the first 4 KB so the loader
        // doesn't poke ROM bytes into user RAM at $B000-$BFFF (the old
        // "best effort" branch landed loadAddr there, which clobbered
        // the user-RAM region with whatever pad bytes the file carried).
        loadAddr  = 0xC000;
        skipBytes = 0x1000;
    } else if (size == 32 * 1024) {
        loadAddr = 0xC000;
        iieFromUpper16K = !pickLower16KFor32K;
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

    // Slice out the firmware payload — for 32 KB dumps this drops the
    // unused half (upper for //c-style, lower for //e-style). After
    // this, `payload` is a 16 KB block that covers $C000-$FFFF (or
    // 12 KB for II+, or whatever the user gave us in the best-effort
    // branch).
    const uint8_t* payload      = buf.data() + skipBytes;
    size_t         payloadSize  = size - skipBytes;
    iicHasAltBank = false;       // reset; only //c-style 32 KB sets it
    iicRomBank    = false;
    if (size == 32 * 1024) {
        if (iieFromUpper16K) {
            payload     = buf.data() + 0x4000;   // //e layout
        } else {
            payload     = buf.data();            // //c bank 0 (active at reset)
            // Stash bank 1 (upper 16 KB) for the $C028 ROMBANK toggle.
            // Bytes 0x000-0x0FF (the $C000-$C0FF soft-switch shadow) are
            // never returned — reads of that range always route through
            // softSwitchAccess regardless of the active bank. Bytes
            // 0x100-0xFFF mirror $C100-$CFFF (alt internal I/O ROM);
            // bytes 0x1000-0x3FFF mirror $D000-$FFFF (alt firmware).
            std::copy(buf.data() + 0x4000,
                      buf.data() + 0x8000,
                      iicAltFirmware.begin());
            iicHasAltBank = true;
            // //c boots with INTCXROM forced on (MAME apple2e.cpp:1273).
            // applyProfile calls resetSoftSwitches BEFORE loadAppleIIRom,
            // so the resetSoftSwitches MF_INTCXROM hook doesn't catch
            // the freshly-flipped iicHasAltBank — set it here too.
            iieMemMode |= MF_INTCXROM;
        }
        payloadSize = 0x4000;
    }

    if (iieMode && payloadSize == 16 * 1024) {
        // IIe split: bytes 0x0000-0x00FF map to $C000-$C0FF (I/O page,
        // ignored — those addresses are soft switches, not ROM). Bytes
        // 0x0100-0x0FFF go into the internal I/O ROM, callable via
        // INTCXROM=on or SLOTC3ROM=off (slot 3 only). Bytes 0x1000-0x3FFF
        // load into $D000-$FFFF as the main Applesoft + Monitor ROM.
        for (size_t i = 0x100; i < 0x1000; ++i) {
            internalIORom[i] = payload[i];
        }
        for (size_t i = 0x1000; i < payloadSize; ++i) {
            uint16_t addr = static_cast<uint16_t>(0xC000 + i);
            mem[addr] = payload[i];
        }
    } else {
        // II+ path (or non-16-KB): linear load, skipping the I/O page so
        // soft switches keep working when a 16 KB II+ image is provided.
        for (size_t i = 0; i < payloadSize; ++i) {
            uint16_t addr = static_cast<uint16_t>(loadAddr + i);
            if (addr >= 0xC000 && addr <= 0xC0FF) continue;
            mem[addr] = payload[i];
        }
    }
    pom2::log().info("ROM", std::string("Loaded ") + filename + " at $" +
                     [&]{ char b[8]; std::snprintf(b, 8, "%04X", loadAddr); return std::string(b); }() +
                     (iieMode ? " (IIe)" : ""));
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
    if (!f) return 0;

    // Normalize to AppleWin's "csbits" convention so the renderer can
    // treat all ROM dumps uniformly:
    //   * Each byte represents one row of 7 visible pixels.
    //   * Bit 0 = leftmost pixel; 1 = lit pixel.
    //   * For codes $00-$3F (inverse range), the stored pattern is
    //     pre-flipped so it already matches inverse video (white BG,
    //     dark glyph). For codes $40-$7F, the stored pattern is the
    //     non-flashing (= normal-looking) glyph; the renderer XORs
    //     with 0x7F when the flash phase is on.
    //
    // 2K II/II+ ROM (e.g. AppleWin's `Apple2_Video.rom`):
    //   - For codes $00-$7F (offsets 0x000-0x3FF): if bit 7 of byte is
    //     0, XOR low 7 bits with 0x7F. (Inverse range bytes have bit
    //     7 = 0; XOR gives the inverse-display pattern. Flashing range
    //     bytes have bit 7 = 1 and stay as the normal-display pattern.)
    //   - All bytes: reverse the low 7 bits (Apple II video shift
    //     register reads MSB-first, so the ROM stores bit 6 = leftmost;
    //     after reverse we end up with bit 0 = leftmost).
    //
    // 4K IIe Enhanced ROM (e.g. AppleWin's `Apple2e_Enhanced_Video.rom`):
    //   - The ROM stores pixels with inverted polarity (1 = OFF) and
    //     bit 0 = leftmost natively. XOR every byte with 0xFF flips to
    //     1 = ON; no reverse needed.
    if (size == 2048) {
        for (size_t i = 0; i < 2048; ++i) {
            uint8_t n = characterRom[i];
            if (i < 1024 && !(n & 0x80)) n ^= 0x7F;
            uint8_t d = 0;
            for (int j = 0; j < 7; ++j) {
                d = static_cast<uint8_t>((d << 1) | (n & 1));
                n >>= 1;
            }
            characterRom[i] = d;
        }
    } else if (size == 4096) {
        for (size_t i = 0; i < 4096; ++i) {
            characterRom[i] ^= 0xFF;
        }
    }
    // 8K (PAL or langsw variants) not preprocessed yet.
    pom2::log().info("ROM",
        "Loaded char ROM (" + std::to_string(size) + " B): " + filename);
    return 1;
}

void Memory::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    cycleCounter += cycles;
    if (cassette) cassette->advanceCycles(cycles);
    slots.advanceCycles(cycles);

    // Scanline-accurate VBL transition detection. Apple II video timing:
    // 262 NTSC scanlines × 65 CPU cycles each. Visible: 0..191. VBL:
    // 192..261. The "long cycle" every line (1 extra cycle every 65) is
    // not modelled; the nominal 65 cycles/line is close enough.
    constexpr uint64_t kCyclesPerScanline = 65;
    constexpr uint64_t kScanlinesPerFrame = 262;
    constexpr uint64_t kVisibleScanlines  = 192;
    const uint64_t scanline = (cycleCounter / kCyclesPerScanline) % kScanlinesPerFrame;
    const bool nowActive = scanline < kVisibleScanlines;

    // Edge: active video → VBL. On a real IIe, `$C05B` (EnVBL) raises
    // the IRQ line each frame, but only when the IOUDIS register is
    // enabled (the boot-time default). With IOUDIS disabled the same
    // address is the AN1 annunciator (legacy II/II+ behaviour) and
    // many programs poke $C05B for paddle / annunciator reasons —
    // raising an IRQ in that case crashes ProDOS (no handler installed).
    // Until POM2 models IOUDIS we keep the pending flag for software
    // that polls it via $C019 read but never assert the CPU IRQ line.
    if (vblWasActive && !nowActive) {
        if (iieMode && vblIrqMask) {
            vblIrqPending = true;
            // (Intentionally NOT calling cpu->setIRQ — see comment.)
        }
    }
    vblWasActive = nowActive;
}

void Memory::resetSoftSwitches()
{
    std::lock_guard<std::mutex> lk(stateMutex);
    display = DisplayState{};
    lcReadRam     = false;
    lcWriteEnable = false;
    lcBank2Active = true;
    lcPrewrite    = false;
    iieMemMode    = 0;
    intC8Rom      = false;   // //e expansion-window auto-INTCXROM flip-flop
    iicRomBank    = false;   // //c boots with bank 0 (the cold-start bank)
    // //c boots with INTCXROM forced on (MAME `apple2e.cpp:1273`). The
    // softswitch is software-toggleable but internal ROM stays mapped
    // either way (see the iicHasAltBank gate in memRead). Setting it
    // here keeps $C015 (RDCXROM) consistent with what real //c
    // firmware sees when probing the switch.
    if (iicHasAltBank) iieMemMode |= MF_INTCXROM;
    // RamWorks III — MAME `a2eramworks3.cpp:65-68 device_reset` snaps
    // `m_bank = 0` on every reset. Match that: swap the visible aux
    // back to bank 0 so Ctrl-Reset / F12 don't leave the user looking
    // at whatever bank software last selected. Data in all banks is
    // preserved (reset clears the bank selector, not the DRAM).
    if (iieMode && ramWorksBanks_ > 1) {
        ramWorksSwapToBank(0);
    }
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
    if (iieMode) {
        aux.fill(0);
        auxLcBank1.fill(0);
        auxLcBank2.fill(0);
        auxLcHigh.fill(0);
        // RamWorks III backing — wipe every bank slot, snap back to
        // bank 0 (MAME `device_reset`-equivalent, plus a cold RAM
        // wipe which `device_reset` itself doesn't do).
        if (ramWorksBanks_ > 1) {
            std::fill(ramWorksBacking_.begin(),
                      ramWorksBacking_.end(), 0u);
            ramWorksBank_ = 0;
        }
    }
}

void Memory::setIIEMode(bool on)
{
    iieMode = on;
    iieMemMode = 0;
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        display.altChar     = false;
        display.eightyStore = false;
        display.eightyCol   = false;
        display.dhgr        = false;
    }
    // Drop any RamWorks backing when leaving IIe (the card is aux-slot
    // only). Re-enabling IIe leaves the user setting to MainWindow.
    if (!on) {
        ramWorksBanks_ = 1;
        ramWorksBank_  = 0;
        ramWorksBacking_.clear();
        ramWorksBacking_.shrink_to_fit();
    }
}

void Memory::setRamWorksBanks(uint32_t banks)
{
    if (banks < 1) banks = 1;
    if (banks > kRamWorksMaxBanks) banks = kRamWorksMaxBanks;
    ramWorksBanks_ = banks;
    ramWorksBank_  = 0;
    // Backing holds ONE slot per bank (including bank 0 — we snapshot
    // the visible buffers into it whenever leaving bank 0). When
    // banks == 1 the backing is empty — stock IIe path, no swap.
    if (banks > 1) {
        ramWorksBacking_.assign(
            static_cast<size_t>(banks) * kRamWorksBankStride, 0u);
    } else {
        ramWorksBacking_.clear();
        ramWorksBacking_.shrink_to_fit();
    }
}

// MAME `a2eramworks3.cpp:108-115 write_c07x`: bank index = data & 0x7F.
// One backing slot per bank (including bank 0). Swap is symmetric:
// snapshot visible → backing[prev], advance ramWorksBank_, load
// backing[curr] → visible. The visible buffers (`aux`, `auxLcBank1/2`,
// `auxLcHigh`) always reflect the active bank so the rest of Memory.cpp
// reads them directly without bank-aware indexing.
void Memory::ramWorksSwapToBank(uint8_t newBank)
{
    if (ramWorksBanks_ <= 1) return;
    // Clamp to populated banks via modulo. MAME does not clamp (it
    // allocates a fixed 8 MB array and reads garbage for unpopulated
    // slots — UB-adjacent in C++); we wrap. On a real RamWorks III with
    // fewer than 128 banks installed, the chip-select decoder aliases
    // higher-bank writes to populated banks anyway.
    newBank = static_cast<uint8_t>(newBank % ramWorksBanks_);
    if (newBank == ramWorksBank_) return;

    const size_t stride = kRamWorksBankStride;
    uint8_t* prev = ramWorksBacking_.data()
                  + static_cast<size_t>(ramWorksBank_) * stride;
    std::memcpy(prev,             aux.data(),         0x10000);
    std::memcpy(prev + 0x10000,   auxLcBank1.data(),  0x1000);
    std::memcpy(prev + 0x11000,   auxLcBank2.data(),  0x1000);
    std::memcpy(prev + 0x12000,   auxLcHigh.data(),   0x2000);

    ramWorksBank_ = newBank;

    const uint8_t* curr = ramWorksBacking_.data()
                        + static_cast<size_t>(newBank) * stride;
    std::memcpy(aux.data(),        curr,             0x10000);
    std::memcpy(auxLcBank1.data(), curr + 0x10000,   0x1000);
    std::memcpy(auxLcBank2.data(), curr + 0x11000,   0x1000);
    std::memcpy(auxLcHigh.data(),  curr + 0x12000,   0x2000);
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

size_t Memory::pasteRawKeys(const char* data, size_t length)
{
    if (!data || length == 0) return 0;
    std::lock_guard<std::mutex> lk(kbMutex);
    size_t queued = 0;
    for (size_t i = 0; i < length && queued < kPasteMaxChars; ++i) {
        const uint8_t b = static_cast<uint8_t>(data[i]) & 0x7F;
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

uint8_t Memory::softSwitchAccess(uint16_t addr, bool isWrite, uint8_t writeVal)
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

    // Keyboard latch + IIe paging soft switches at $C000-$C00F.
    //
    // MAME's `apple2.cpp:548` mirrors $C000 across $C001-$C00F via
    // `.mirror(0xf)`, and `apple2e.cpp:1825-1828` does the same with
    // `if((offset & 0xf0) == 0) return m_transchar | m_strobe;`. So on
    // either II+ or IIe, READS of $C000-$C00F return the keyboard
    // latch — they do NOT toggle the IIe paging soft switches. WRITES
    // to $C001-$C00F dispatch to the IIe handler (writes-only on real
    // hardware).
    //
    // BUT $C00C/$C00D drive the Le Chat Mauve / Video-7 RGB FIFO data
    // line — and those cards sniff the bus regardless of read/write
    // direction. On a IIe the broadcast is folded into
    // `iieHandleSoftSwitch`; on a II+ we run the broadcast directly
    // here because there's no IIe handler to do it for us.
    if (low <= 0x0F) {
        if (!iieMode && (low == 0x0C || low == 0x0D)) {
            {
                std::lock_guard<std::mutex> lk(stateMutex);
                display.eightyCol = (low == 0x0D);
            }
            slots.broadcastVideoSwitch(addr);
        }
        if (isWrite && iieMode) {
            iieHandleSoftSwitch(addr);
        }
        return kbLatch;
    }
    // Keyboard strobe clear — read or write.
    if (low == 0x10) {
        // IIe: bit 7 reflects "any key down" (MAME `apple2e.cpp:1833`:
        // `m_transchar | (m_anykeydown ? 0x80 : 0x00)`). POM2 doesn't
        // model key-release events separately, so we approximate
        // "any-key-down" with the pre-clear strobe state — that's what
        // software typically polls $C010 for ("is the user still
        // holding a key?"). On II+ the strobe-clear semantic is
        // historical: bit 7 LOW after clear.
        const bool wasReady = keyReady;
        clearKeyStrobe();
        if (iieMode) {
            return static_cast<uint8_t>(
                (kbLatch & 0x7F) | (wasReady ? 0x80 : 0x00));
        }
        return kbLatch & 0x7F;
    }
    // Language Card status mirrors (Apple IIe-compatible; harmless on II+).
    // In iieMode the low 7 bits carry m_transchar (last keyboard char)
    // per MAME `apple2e.cpp:1842-1871`. On II+ they're zero (real II+
    // would return the floating bus; not modelled here).
    if (low == 0x11 || low == 0x12) {
        const bool on = (low == 0x11) ? lcBank2Active : lcReadRam;
        uint8_t low7 = 0;
        if (iieMode) {
            std::lock_guard<std::mutex> lk(kbMutex);
            low7 = static_cast<uint8_t>(lastKey & 0x7F);
        }
        return static_cast<uint8_t>((on ? 0x80 : 0x00) | low7);
    }

    // (The IIe paging dispatch for $C001-$C00F now lives inside the
    // `low <= 0x0F` block above — gated on `isWrite` so reads of those
    // addresses don't accidentally flip paging bits.)

    // IIe status reads at $C013-$C018 + $C01E-$C01F (high bit reflects
    // the matching MF_* / DisplayState bit).
    if (iieMode && low >= 0x13 && low <= 0x1F) {
        const uint8_t s = iieReadStatus(addr);
        if (s != 0xFE) return s;  // 0xFE = sentinel for "not handled here"
    }
    // VBL (vertical blank) strobe — IIe-only register. Scanline-accurate
    // Apple II frame: 262 scanlines × 65 cycles. Visible video =
    // 0..191, VBL = 192..261. Bit 7 of $C019 reflects the active-video
    // state per MAME `apple2e.cpp:2107` convention: HIGH during active,
    // LOW during VBL. II/II+ doesn't decode $C019 at all — the address
    // falls through to the floating bus per MAME `apple2.cpp` (no
    // $C019 case in `do_io`). Without the iieMode gate, software
    // running on II+ that probes $C019 (e.g. some ProDOS detection
    // routines) would see a deterministic scanline-derived value
    // instead of the random-ish video DMA byte real hardware returns.
    //
    // **$C019 read does NOT clear the VBL IRQ** — per Apple IIc
    // Technical Note #9 (and MAME `apple2e.cpp:2244` comment "does not
    // reset"). The clear path is the $C05A AN1 write (IIc/IIc+; POM2
    // overlays the same on IIe as documented in the $C058-$C05D block
    // below). Earlier POM2 versions cleared the IRQ on every $C019
    // read, contradicting Tech Note #9.
    if (iieMode && low == 0x19) {
        constexpr uint64_t kCyclesPerScanline = 65;
        constexpr uint64_t kScanlinesPerFrame = 262;
        constexpr uint64_t kVisibleScanlines  = 192;
        const uint64_t scanline = (cycleCounter / kCyclesPerScanline) % kScanlinesPerFrame;
        const bool nowActive = scanline < kVisibleScanlines;
        // IIe: OR m_transchar into the low 7 bits (MAME
        // `apple2e.cpp:2107`). II+ has no $C019; leave low 7 = 0.
        uint8_t low7 = 0;
        if (iieMode) {
            std::lock_guard<std::mutex> lk(kbMutex);
            low7 = static_cast<uint8_t>(lastKey & 0x7F);
        }
        return static_cast<uint8_t>((nowActive ? 0x80 : 0x00) | low7);
    }

    // Annunciators AN0 ($C058/9), AN1 ($C05A/B), AN2 ($C05C/D). Each
    // pair toggles a dedicated output line on the game I/O connector
    // (MAME `apple2e.cpp:1750-1773`). POM2 doesn't wire those lines
    // anywhere yet, but software still expects the soft switch to
    // *swallow* the access (return zero / floating bus, no side
    // effects on display). The IIe-specific note: MAME treats $C05A/B
    // as plain AN1 toggles on IIe — VBL IRQ masking lives on IIc/IIc+
    // only (`apple2e.cpp:2057-2065` `lower_irq` is `m_isiic ||
    // m_isace500`). POM2 historically wired the mask in IIe mode for
    // convenience (and `tests/vbl_smoke_test.cpp` pins it that way);
    // we keep that behaviour AS AN OVERLAY on top of the AN1 state
    // tracking so a future IIc port can drop the overlay without
    // disturbing the AN-state model.
    if (low >= 0x58 && low <= 0x5D) {
        const bool on = (low & 1) != 0;
        switch ((low - 0x58) >> 1) {
            case 0: an0 = on; break;
            case 1: an1 = on; break;
            case 2: an2 = on; break;
        }
        if (iieMode && (low == 0x5A || low == 0x5B)) {
            // POM2 overlay: $C05A/B doubles as VBL IRQ mask in IIe.
            // Strictly speaking that's an IIc/IIc+ feature in MAME;
            // we keep it here so existing software that relies on the
            // overlay (and `vbl_smoke_test.cpp`) keeps working.
            if (low == 0x5A) {
                vblIrqMask = false;
                if (vblIrqPending) {
                    vblIrqPending = false;
                    if (cpu) cpu->setIrqLine(M6502::IRQ_SRC_VBL, false);
                }
            } else {
                vblIrqMask = true;
            }
        }
        return 0;
    }

    // $C040 utility strobe — MAME `apple2e.cpp:1711-1716` pulses the
    // game I/O connector's STRB pin (high → low → high) on every
    // access. The only consumer on a real Apple II is a paddle-style
    // peripheral with its own latch, none of which POM2 currently
    // emulates. We swallow the access so the read doesn't fall
    // through to floating bus (which would also be fine, but the
    // explicit return makes intent clear).
    if (low == 0x40) return 0;

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

    // 80COL ($C00C off / $C00D on). On a II/II+ this is purely a
    // hook for Le Chat Mauve / Video-7 RGB cards co-opting it as the
    // data line of their 2-bit FIFO mode register (AN3 = clock).
    // On a IIe `iieHandleSoftSwitch` (called above for `low <= 0x0F`)
    // already updated MF_80COL + display.eightyCol AND ran
    // broadcastVideoSwitch for us, so we MUST NOT re-fire here — that
    // would double-clock the FIFO. Gate on !iieMode.
    if (!iieMode && (low == 0x0C || low == 0x0D)) {
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
    //
    // On a IIe the same two addresses are DHIRESON ($C05E) / DHIRESOFF
    // ($C05F): they enable / disable double hi-res mode. The polarity is
    // OPPOSITE the AN3 line (AN3 high ↔ DHGR off), so we track DHGR as a
    // separate bit instead of inverting at the read site. The Le Chat
    // Mauve hook below still fires on every access so its FIFO continues
    // to clock on a IIe with the card plugged.
    if (low == 0x5E || low == 0x5F) {
        {
            std::lock_guard<std::mutex> lk(stateMutex);
            display.an3 = (low == 0x5F);
            if (iieMode) display.dhgr = (low == 0x5E);
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

    // Apple //c ROMBANK ($C020-$C02F): when a two-bank //c firmware
    // dump is loaded, any access in the **entire $C020-$C02F range**
    // toggles which 16 KB bank is mapped into the motherboard ROM
    // window ($C100-$CFFF under INTCXROM, $D000-$FFFF under LC ROM).
    // MAME `apple2e.cpp:1894-1922` flips `m_romswitch` on
    // `(offset & 0xf0) == 0x20` when `m_isiic`; the Apple IIc Tech Ref
    // 2e edition lists the softswitch at "$c02x" range, not just $C028.
    // Both reads and writes trigger the toggle. The //c has no cassette
    // either, so MAME suppresses cassette_toggle on //c — POM2 does the
    // same below (gated on !iicHasAltBank).
    if (low >= 0x20 && low <= 0x2F) {
        if (iicHasAltBank) {
            iicRomBank = !iicRomBank;
            return 0;
        }
        // Cassette OUTPUT toggle (II/II+/IIe path): the Monitor's WRITE
        // routine ($FECD) loops on BIT $C020 to drive the magnetic head
        // with 770 Hz / 1 kHz / 2 kHz square waves encoding sync, ones
        // and zeroes.
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

    // Push-buttons + paddle inputs at $C061-$C067, mirrored across
    // $C068-$C06F (MAME `apple2.cpp:554` `.mirror(0x8)` and
    // `apple2e.cpp:1889/1903/1909/1915/1919/1923/1927`). Real hardware
    // ORs the floating-bus byte into the low 7 bits (Beagle Bros and
    // demoscene RNGs depend on this). STATEREG / band-select at
    // $C068+ are IIgs-only; POM2 (II/II+/IIe-only) shouldn't expose
    // them.
    if (low >= 0x61 && low <= 0x6F && low != 0x6A
        && !(low >= 0x68 && low <= 0x6F && low == 0x68 /* IIc/IIgs */)) {
        // (Empty guards — see below for the unified handler.)
    }
    if (low >= 0x61 && low <= 0x6F) {
        const uint8_t mirrored = static_cast<uint8_t>(0x60 | (low & 0x07));
        const uint8_t bit7 = [&]() -> uint8_t {
            switch (mirrored) {
                case 0x61: return (paddleButton[0] || openAppleKey.load())  ? 0x80 : 0x00;
                case 0x62: return (paddleButton[1] || solidAppleKey.load()) ? 0x80 : 0x00;
                case 0x63: return (paddleButton[2]
                                  || (iieMode && shiftKey.load()))          ? 0x80 : 0x00;
                case 0x64: case 0x65: case 0x66: case 0x67: {
                    const int idx = mirrored - 0x64;
                    const uint64_t elapsed = cycleCounter - paddleLatchCycle;
                    // ~11 cycles per paddle-value step (max ~2816c).
                    const uint64_t threshold =
                        static_cast<uint64_t>(paddleValue[idx]) * 11;
                    return (elapsed < threshold) ? 0x80 : 0x00;
                }
                default: return 0;  // $C060 already handled above
            }
        }();
        return static_cast<uint8_t>(bit7 | (floatingBus() & 0x7F));
    }

    // Paddle-trigger reset ($C070-$C07F mirrored). MAME `apple2.cpp:555`
    // `.mirror(0xf)`; the read returns floating bus (used as a poor
    // RNG seed by many games). Real silicon also implements a 558
    // monostable one-shot semantic (re-strobing during the count
    // doesn't restart the timer); POM2 reloads unconditionally — the
    // simpler model passes every game we've tested.
    //
    // RamWorks III (IIe aux-slot card) sniffs writes to $C071/3/5/7
    // on the same address window. MAME `a2eramworks3.cpp:108-115`
    // predicate `(offset & 0x9) == 1` over the low nibble selects
    // those four addresses; the data byte's low 7 bits (`data & 0x7F`,
    // line 113) is the new bank index. Bank switch and paddle reset
    // both fire on the same access — they share the bus, neither
    // shadows the other.
    if (low >= 0x70 && low <= 0x7F) {
        paddleLatchCycle = cycleCounter;
        if (isWrite && iieMode && ramWorksBanks_ > 1
            && (low & 0x09) == 0x01) {
            ramWorksSwapToBank(static_cast<uint8_t>(writeVal & 0x7F));
        }
        return isWrite ? 0 : floatingBus();
    }

    // Unknown soft switch — Apple II hardware floats the bus. For reads,
    // return whatever the video DMA is currently fetching (matches real
    // hardware so software that uses $C0xx as a poor RNG, or that BMI/BPL
    // on a status read's low 7 bits, behaves correctly). Writes don't
    // care about the return value.
    return isWrite ? 0 : floatingBus();
}

void Memory::iieHandleSoftSwitch(uint16_t addr)
{
    // $C000-$C00F: 80STORE / RAMRD / RAMWRT / INTCXROM / ALTZP / SLOTC3ROM
    // / 80COL / ALTCHAR. Even byte = OFF (clear bit), odd byte = ON.
    const uint8_t low = static_cast<uint8_t>(addr & 0x0F);
    const bool   on  = (low & 1) != 0;
    uint16_t flag = 0;
    switch (low >> 1) {
        case 0: flag = MF_80STORE;   break;
        case 1: flag = MF_RAMRD;     break;
        case 2: flag = MF_RAMWRT;    break;
        case 3: flag = MF_INTCXROM;  break;
        case 4: flag = MF_ALTZP;     break;
        case 5: flag = MF_SLOTC3ROM; break;
        case 6: flag = MF_80COL;     break;
        case 7: flag = MF_ALTCHAR;   break;
    }
    if (on) iieMemMode |= flag;
    else    iieMemMode &= static_cast<uint16_t>(~flag);

    // Mirror display-relevant bits into DisplayState so Apple2Display can
    // pick them up via its single getDisplayState() snapshot per frame.
    if (flag == MF_80STORE || flag == MF_80COL || flag == MF_ALTCHAR) {
        std::lock_guard<std::mutex> lk(stateMutex);
        display.eightyStore = (iieMemMode & MF_80STORE) != 0;
        display.eightyCol   = (iieMemMode & MF_80COL)   != 0;
        display.altChar     = (iieMemMode & MF_ALTCHAR) != 0;
    }
    // Le Chat Mauve / Video-7 RGB FIFO clocking — for the 80COL pair
    // ($C00C/D) we need to forward the data-bit edge to plugged video
    // cards even in IIe mode. AN3 ($C05E/F) takes a separate path
    // outside this handler. Without this broadcast, software that uses
    // 80COL toggles on a IIe to drive the Le Chat Mauve mode FIFO
    // would silently stop clocking.
    if (flag == MF_80COL) {
        slots.broadcastVideoSwitch(addr);
    }
}

uint8_t Memory::iieReadStatus(uint16_t addr) const
{
    const uint8_t low = static_cast<uint8_t>(addr & 0xFF);
    DisplayState ds;
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        ds = display;
    }
    // MAME `apple2e.cpp:1842-1871` `c000_r`: every status read in
    // $C011-$C01F returns `(bit ? 0x80 : 0x00) | m_transchar`. The low
    // 7 bits carry the last latched keyboard character — software like
    // Beagle Bros' Pro-Byter, Print Shop, and the IIe Self-Test rely on
    // this to read "key + status flag" in one byte. POM2's
    // `lastKey` is the same latch as MAME's m_transchar.
    uint8_t transchar = 0;
    {
        std::lock_guard<std::mutex> lk(kbMutex);
        transchar = static_cast<uint8_t>(lastKey & 0x7F);
    }
    auto bit = [transchar](bool on) -> uint8_t {
        return static_cast<uint8_t>((on ? 0x80 : 0x00) | transchar);
    };
    switch (low) {
        case 0x13: return bit((iieMemMode & MF_RAMRD)     != 0);  // RDRAMRD
        case 0x14: return bit((iieMemMode & MF_RAMWRT)    != 0);  // RDRAMWRT
        case 0x15: return bit((iieMemMode & MF_INTCXROM)  != 0);  // RDCXROM
        case 0x16: return bit((iieMemMode & MF_ALTZP)     != 0);  // RDALTZP
        case 0x17: return bit((iieMemMode & MF_SLOTC3ROM) != 0);  // RDC3ROM
        case 0x18: return bit((iieMemMode & MF_80STORE)   != 0);  // RD80STORE
        case 0x1A: return bit(ds.textMode);                       // RDTEXT
        case 0x1B: return bit(ds.mixedMode);                      // RDMIXED
        case 0x1C: return bit(ds.page2);                          // RDPAGE2
        case 0x1D: return bit(ds.hiRes);                          // RDHIRES
        case 0x1E: return bit((iieMemMode & MF_ALTCHAR)   != 0);  // RDALTCHAR
        case 0x1F: return bit((iieMemMode & MF_80COL)     != 0);  // RD80COL
    }
    return 0xFE;  // not a status read — let the caller continue
}

uint8_t Memory::iieMemRead(uint16_t addr)
{
    // Routing rules (see header):
    //   $0000-$01FF      ALTZP            → aux else main
    //   $0200-$03FF      RAMRD            → aux else main
    //   $0400-$07FF      80STORE on       → PAGE2 picks aux/main; else RAMRD
    //   $0800-$1FFF      RAMRD            → aux else main
    //   $2000-$3FFF      80STORE+HIRES on → PAGE2 picks aux/main; else RAMRD
    //   $4000-$BFFF      RAMRD            → aux else main
    //
    // Mutex note: `display.page2` and `display.hiRes` are read here
    // without holding `stateMutex`. That is intentionally safe in our
    // threading model: both the writers (softSwitchAccess, iieHandle-
    // SoftSwitch, resetSoftSwitches) and this reader run on the CPU
    // worker thread — they cannot race against each other. The UI
    // thread always reads display state through `getDisplayState()`,
    // which copies the struct under the mutex. TSAN may flag the read
    // formally because the writers DO take the mutex, but no actual
    // race exists. Adding a per-access mutex acquire here would tank
    // performance (one lock per emulated bus cycle).
    if (addr < 0x0200) {
        return (iieMemMode & MF_ALTZP) ? aux[addr] : mem[addr];
    }
    const bool ramrd = (iieMemMode & MF_RAMRD) != 0;
    if (addr >= 0x0400 && addr <= 0x07FF) {
        if (iieMemMode & MF_80STORE) {
            const bool page2 = display.page2;
            return page2 ? aux[addr] : mem[addr];
        }
        return ramrd ? aux[addr] : mem[addr];
    }
    if (addr >= 0x2000 && addr <= 0x3FFF) {
        if ((iieMemMode & MF_80STORE) && display.hiRes) {
            const bool page2 = display.page2;
            return page2 ? aux[addr] : mem[addr];
        }
        return ramrd ? aux[addr] : mem[addr];
    }
    return ramrd ? aux[addr] : mem[addr];
}

void Memory::iieMemWrite(uint16_t addr, uint8_t value)
{
    if (addr < 0x0200) {
        if (iieMemMode & MF_ALTZP) aux[addr] = value;
        else                       mem[addr] = value;
        return;
    }
    const bool ramwrt = (iieMemMode & MF_RAMWRT) != 0;
    if (addr >= 0x0400 && addr <= 0x07FF) {
        if (iieMemMode & MF_80STORE) {
            if (display.page2) aux[addr] = value;
            else               mem[addr] = value;
            return;
        }
        if (ramwrt) aux[addr] = value;
        else        mem[addr] = value;
        return;
    }
    if (addr >= 0x2000 && addr <= 0x3FFF) {
        if ((iieMemMode & MF_80STORE) && display.hiRes) {
            if (display.page2) aux[addr] = value;
            else               mem[addr] = value;
            return;
        }
        if (ramwrt) aux[addr] = value;
        else        mem[addr] = value;
        return;
    }
    if (ramwrt) aux[addr] = value;
    else        mem[addr] = value;
}

uint8_t Memory::languageCardSwitchAccess(uint16_t addr, bool isWrite)
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
    // Pre-write latch: armed only by READ-cycle accesses (`LDA $C08x`,
    // `BIT $C08x`). Any STORE to $C08x clears the latch — MAME
    // ramcard16k.cpp:61-64 + apple2e.cpp:1515-1520 "any write disables
    // pre-write". This is what makes the classic enable sequence
    // `LDA $C081 / LDA $C081` work (two reads arm + commit) while
    // `STA $C081 / STA $C081` does NOT enable RAM writes.
    lcPrewrite = isWrite ? false : writeCandidate;

    // The card itself does not drive the data lines for $C08x — the byte
    // the CPU reads is whatever the video DMA last latched onto the bus.
    return floatingBus();
}

uint8_t Memory::floatingBus() const
{
    // Verbatim port of MAME `apple2video.cpp:124-201 scanner_address`.
    // Input: h_clock [0..64] (active video from 25), v_clock [0..261]
    // (active video from 0). Output: 16-bit DRAM address the video
    // scanner is currently fetching. Used by reads of unimplemented
    // soft switches ($C040, $C050-$C05F mirrors during VBL, $C019 in
    // II/II+, etc.) which let the floating bus byte through. Software
    // using this as an RNG seed (Beagle Bros copy protection, some
    // demos) needs bit-exact replication of the scanner counter.
    //
    // The earlier POM2 implementation built the address from a
    // text/HGR row-interleave formula; that gave the same byte during
    // active video but diverged during HBL (where MAME's `addend0=0x0D
    // + h-carries` produces the "$1000 phantom row" effect) and on
    // page-2 HGR (m_hgr2 base differs).
    constexpr uint64_t kCyclesPerLine  = 65;
    constexpr uint64_t kLinesPerFrame  = 262;
    constexpr uint64_t kCyclesPerFrame = kCyclesPerLine * kLinesPerFrame;

    const uint64_t cyc     = cycleCounter % kCyclesPerFrame;
    const int      v_clock = static_cast<int>(cyc / kCyclesPerLine);  // 0..261
    const int      h_clock = static_cast<int>(cyc % kCyclesPerLine);  // 0..64

    DisplayState ds;
    {
        std::lock_guard<std::mutex> lk(stateMutex);
        ds = display;
    }
    int Hires = (ds.hiRes && !ds.textMode) ? 1 : 0;
    const int Mixed = ds.mixedMode ? 1 : 0;
    const int Page2 = ds.page2    ? 1 : 0;

    // MAME `apple2video.cpp:140`: two 0-states ([0, 0..63]).
    const int h_state = h_clock - (h_clock > 0);
    const int h_0 = (h_state >> 0) & 1;
    const int h_1 = (h_state >> 1) & 1;
    const int h_2 = (h_state >> 2) & 1;
    const int h_3 = (h_state >> 3) & 1;
    const int h_4 = (h_state >> 4) & 1;
    const int h_5 = (h_state >> 5) & 1;

    // MAME `apple2video.cpp:149`: V[543210CBA] = 100000000 = 256+v.
    // The overflow compensation uses screen().height() in MAME; POM2's
    // frame is 262 lines so we subtract the screen height when v wraps.
    int v_state = 256 + v_clock;
    if (v_clock >= 256) v_state -= static_cast<int>(kLinesPerFrame);
    const int v_A = (v_state >> 0) & 1;
    const int v_B = (v_state >> 1) & 1;
    const int v_C = (v_state >> 2) & 1;
    const int v_0 = (v_state >> 3) & 1;
    const int v_1 = (v_state >> 4) & 1;
    const int v_2 = (v_state >> 5) & 1;
    const int v_3 = (v_state >> 6) & 1;
    const int v_4 = (v_state >> 7) & 1;

    // Mixed-mode bottom 4 text rows: HGR off when Mixed && v_4 && v_2.
    if (Hires && Mixed && v_4 && v_2) Hires = 0;

    const int addend0 = 0x0D;
    const int addend1 = (h_5 << 2) | (h_4 << 1) | (h_3 << 0);
    const int addend2 = (v_4 << 3) | (v_3 << 2) | (v_4 << 1) | (v_3 << 0);
    const int sum     = (addend0 + addend1 + addend2) & 0x0F;

    uint16_t address = 0;
    address |= static_cast<uint16_t>(h_0 << 0);
    address |= static_cast<uint16_t>(h_1 << 1);
    address |= static_cast<uint16_t>(h_2 << 2);
    address |= static_cast<uint16_t>(sum << 3);
    address |= static_cast<uint16_t>(v_0 << 7);
    address |= static_cast<uint16_t>(v_1 << 8);
    address |= static_cast<uint16_t>(v_2 << 9);
    if (Hires) {
        address |= static_cast<uint16_t>(v_A << 10);
        address |= static_cast<uint16_t>(v_B << 11);
        address |= static_cast<uint16_t>(v_C << 12);
        // HGR page base: $2000 (page 1) or $4000 (page 2). MAME's
        // `m_hgr2` is the page-2 base for IIe; on II/II+ it's the
        // same $4000.
        address |= static_cast<uint16_t>(Page2 ? 0x4000 : 0x2000);
    } else {
        // Text base. MAME also adds 0x1000 during HBL on II/II+ ("Apple
        // II HBL phantom row"); on IIe that bit is suppressed. POM2 is
        // a II/II+/IIe emulator — gate on !iieMode to match the model.
        address |= static_cast<uint16_t>(Page2 ? 0x0800 : 0x0400);
        if (!iieMode && h_clock < 25) {
            address |= 0x1000;
        }
    }
    return mem[address];
}

uint8_t Memory::languageCardRead(uint16_t addr) const
{
    if (!lcReadRam) {
        // //c ROMBANK alt firmware overrides motherboard ROM at $D000-$FFFF.
        // LC RAM path below is unaffected — banking only swaps the ROM side.
        if (iicHasAltBank && iicRomBank)
            return iicAltFirmware[addr - 0xC000];
        return mem[addr];
    }
    const bool useAux = iieMode && (iieMemMode & MF_ALTZP);
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (useAux) return lcBank2Active ? auxLcBank2[off] : auxLcBank1[off];
        return lcBank2Active ? lcBank2[off] : lcBank1[off];
    }
    if (useAux) return auxLcHigh[addr - 0xE000];
    return lcHigh[addr - 0xE000];
}

void Memory::languageCardWrite(uint16_t addr, uint8_t value)
{
    if (!lcWriteEnable) return;
    const bool useAux = iieMode && (iieMemMode & MF_ALTZP);
    if (addr < 0xE000) {
        const uint16_t off = static_cast<uint16_t>(addr - 0xD000);
        if (useAux) {
            if (lcBank2Active) auxLcBank2[off] = value;
            else               auxLcBank1[off] = value;
        } else {
            if (lcBank2Active) lcBank2[off] = value;
            else               lcBank1[off] = value;
        }
        return;
    }
    if (useAux) auxLcHigh[addr - 0xE000] = value;
    else        lcHigh[addr - 0xE000] = value;
}

uint8_t Memory::memRead(uint16_t addr)
{
    // Klaus harness: flat 64 KB RAM, no side effects.
    if (testMode) return mem[addr];

    // Fast path: RAM below $C000. In IIe mode the read may route to aux RAM
    // (RAMRD / ALTZP / 80STORE+PAGE2). In II+ mode, plain main bank.
    if (addr < 0xC000) {
        return iieMode ? iieMemRead(addr) : mem[addr];
    }
    if (addr >= 0xD000) return languageCardRead(addr);

    // $C000-$C07F — built-in I/O page (keyboard, speaker, cassette,
    // display soft switches, paddles).
    if (addr <= 0xC07F) return softSwitchAccess(addr, /*isWrite=*/false, 0);

    // $C080-$C0FF — slot device-select (16 bytes per slot, slot N at
    // $C080+N*16; slot 0 = language card, slots 1-7 = expansion cards).
    if (addr <= 0xC08F) return languageCardSwitchAccess(addr, /*isWrite=*/false);
    if (addr <= 0xC0FF) return slots.deviceSelectRead(addr);

    // $C100-$CFFF — slot ROM dispatch.
    //
    // II+: $C100-$C7FF goes to slot bus, $C800-$CFFF is the shared expansion
    // window owned by the most-recently-selected slot.
    //
    // IIe: INTCXROM=on swallows the entire $C100-$CFFF range into the
    // motherboard internal I/O ROM. Even when INTCXROM=off, $C300-$C3FF
    // is owned by the internal ROM unless SLOTC3ROM=on (so PR#3 reads
    // the IIe 80-col firmware out of the box).
    //
    if (iieMode) {
        // //c hardware (iicHasAltBank): no physical slots — internal
        // ROM is always mapped at $C100-$CFFF regardless of INTCXROM.
        // MAME `apple2e.cpp:1617-1635` (`update_slotrom_banks`) ORs
        // `m_isiic` into every internal-ROM gate; the softswitch is
        // still writable/readable via $C006/$C007/$C015 but has no
        // effect on what actually executes from $CnXX. The //c reset
        // routine at $FA62 immediately `JSR $CE4D` etc. — without this
        // override those addresses would fall through to slot bus
        // (empty → $FF), and the //c never boots.
        if ((iieMemMode & MF_INTCXROM) || iicHasAltBank) {
            if (addr == 0xCFFF) {
                intC8Rom = false;
                slots.deactivateExpansion();
            }
            if (iicHasAltBank && iicRomBank)
                return iicAltFirmware[addr - 0xC000];
            return internalIORom[addr - 0xC000];
        }
        // $C300-$C3FF with SLOTC3ROM=off: return internal 80-col
        // firmware AND auto-enable `intC8Rom` so the firmware's
        // continuation in $C800-$CFFF (JMP $C803/$C87C/$C9B4/...) reads
        // internal ROM instead of slot bus. MAME
        // `apple2e.cpp:c300_int_r`: `m_intc8rom = true; update_slotrom_banks()`.
        if (addr >= 0xC300 && addr <= 0xC3FF &&
            !(iieMemMode & MF_SLOTC3ROM)) {
            intC8Rom = true;
            if (iicHasAltBank && iicRomBank)
                return iicAltFirmware[addr - 0xC000];
            return internalIORom[addr - 0xC000];
        }
        // $C800-$CFFF with `intC8Rom` set: shared expansion window
        // mapped to internal ROM. Reading $CFFF additionally clears
        // `intC8Rom` and releases the slot expansion-ROM owner —
        // MAME `apple2e.cpp:c800_int_r`: `if (offset == 0x7ff) {
        // m_cnxx_slot = CNXX_UNCLAIMED; m_intc8rom = false; ... }`.
        if (intC8Rom && addr >= 0xC800 && addr <= 0xCFFF) {
            const uint8_t v = internalIORom[addr - 0xC000];
            if (addr == 0xCFFF) {
                intC8Rom = false;
                slots.deactivateExpansion();
            }
            return v;
        }
        // $CFFF without intC8Rom set: still release the slot expansion
        // owner — real //e wires the address decode directly to the
        // slot latch reset, bypassing the INTCXROM mux (MAME
        // `apple2e.cpp:2636-2645` `c800_r` always runs the deactivate).
        if (addr == 0xCFFF) {
            slots.deactivateExpansion();
        }
    }
    if (addr <= 0xC7FF) return slots.slotRomRead(addr);
    return slots.expansionRomRead(addr);
}

void Memory::memWrite(uint16_t addr, uint8_t value)
{
    // Klaus harness: flat 64 KB RAM, no side effects.
    if (testMode) { mem[addr] = value; return; }

    // Fast path: writable RAM and Language Card overlay for $D000-$FFFF.
    if (addr < 0xC000) {
        if (!writable[addr]) return;
        if (iieMode) iieMemWrite(addr, value);
        else         mem[addr] = value;
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
            languageCardSwitchAccess(addr, /*isWrite=*/true);
            return;
        }
        slots.deviceSelectWrite(addr, value);
        return;
    }
    // Same $CFFF + INTCXROM deactivate handling as memRead (MAME
    // `apple2e.cpp:2636-2645`). The write goes through to the slot
    // bus regardless so cards that decode their own $C800-$CFFF
    // window (rare on a IIe) still see it.
    if (iieMode && addr == 0xCFFF) {
        slots.deactivateExpansion();
    }
    // Mirror the memRead INTCXROM override for //c: when internal ROM
    // is mapped at $C100-$CFFF, writes are absorbed (real silicon: ROM
    // is read-only, slot bus is not reached). Without this, a //c
    // firmware write into $CnXX would forward to a (non-existent) slot
    // card and possibly latch activeExpansionSlot to a stale value.
    if (iieMode && ((iieMemMode & MF_INTCXROM) || iicHasAltBank)) {
        return;
    }
    if (addr <= 0xC7FF) {
        // Slot ROM is read-only on most cards, but a handful (Mockingboard
        // in particular) decode 6522 VIA MMIO inside the $CnXX window.
        // SlotBus::slotRomWrite forwards to the card and latches the slot
        // as the active expansion-ROM owner (same as a read into the
        // window), so cards that genuinely have read-only ROM still see
        // their slot select on writes.
        slots.slotRomWrite(addr, value);
        return;
    }
    // $C800-$CFFF — expansion ROM, conventionally read-only. Forward
    // through SlotBus so $CFFF disable still works on writes (the
    // address bus is what matters, not the direction).
    slots.expansionRomWrite(addr, value);
}

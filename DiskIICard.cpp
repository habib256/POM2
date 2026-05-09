// POM2 Apple II Emulator
// Copyright (C) 2026

#include "DiskIICard.h"
#include "Logger.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>

namespace {
// ~32 CPU cycles per nibble = 4 µs bit cells × 8 bits at 1.0227 MHz. Real
// hardware varies with the LSS state, but DOS 3.3 / ProDOS only need
// average-rate accuracy because they re-sync on every D5-AA prologue.
constexpr int kCyclesPerNibble  = 32;
constexpr int kQuarterTrackMax  = (DiskImage::kTracks - 1) * 4;  // 136

// Default contents of the Apple Disk II P6 PROM (341-0028-A, 16-sector
// variant). Source: apple2js `disk2.ts` `SEQUENCER_ROM_16`, which is in
// turn transcribed from "Understanding the Apple //e" (Sather), Figure
// 9.11. Same bytes MAME's `wozfdc.cpp` loads from `341-0028-a.rom`.
//
// Used as the fallback when `roms/diskii_p6.rom` is missing — POM2 keeps
// the LSS path active out of the box without depending on a dumped PROM
// file. `loadLssRom()` overrides this default if the user provides a
// verified dump.
//
// 16 rows × 16 cols. Row = current state. Column = (Q7, Q6, QA, PULSE)
// composite — see DiskIICard.h for the full address-bit layout.
constexpr uint8_t kP6RomDefault[256] = {
    // Q7 L (Read)                                     Q7 H (Write)
    // Q6 L (Shift)        Q6 H (Load)         Q6 L (Shift)        Q6 H (Load)
    // QA L     QA H       QA L     QA H       QA L     QA H       QA L     QA H
    // !PULSE: 1 0 1 0     1 0 1 0  1 0 1 0    1 0 1 0  1 0 1 0    1 0 1 0  1 0 1 0
    0x18, 0x18, 0x18, 0x18, 0x0A, 0x0A, 0x0A, 0x0A, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, // 0
    0x2D, 0x2D, 0x38, 0x38, 0x0A, 0x0A, 0x0A, 0x0A, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, 0x28, // 1
    0xD8, 0x38, 0x08, 0x28, 0x0A, 0x0A, 0x0A, 0x0A, 0x39, 0x39, 0x39, 0x39, 0x3B, 0x3B, 0x3B, 0x3B, // 2
    0xD8, 0x48, 0x48, 0x48, 0x0A, 0x0A, 0x0A, 0x0A, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, 0x48, // 3
    0xD8, 0x58, 0xD8, 0x58, 0x0A, 0x0A, 0x0A, 0x0A, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, 0x58, // 4
    0xD8, 0x68, 0xD8, 0x68, 0x0A, 0x0A, 0x0A, 0x0A, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, 0x68, // 5
    0xD8, 0x78, 0xD8, 0x78, 0x0A, 0x0A, 0x0A, 0x0A, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, 0x78, // 6
    0xD8, 0x88, 0xD8, 0x88, 0x0A, 0x0A, 0x0A, 0x0A, 0x08, 0x08, 0x88, 0x88, 0x08, 0x08, 0x88, 0x88, // 7
    0xD8, 0x98, 0xD8, 0x98, 0x0A, 0x0A, 0x0A, 0x0A, 0x98, 0x98, 0x98, 0x98, 0x98, 0x98, 0x98, 0x98, // 8
    0xD8, 0x29, 0xD8, 0xA8, 0x0A, 0x0A, 0x0A, 0x0A, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, 0xA8, // 9
    0xCD, 0xBD, 0xD8, 0xB8, 0x0A, 0x0A, 0x0A, 0x0A, 0xB9, 0xB9, 0xB9, 0xB9, 0xBB, 0xBB, 0xBB, 0xBB, // A
    0xD9, 0x59, 0xD8, 0xC8, 0x0A, 0x0A, 0x0A, 0x0A, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, 0xC8, // B
    0xD9, 0xD9, 0xD8, 0xA0, 0x0A, 0x0A, 0x0A, 0x0A, 0xD8, 0xD8, 0xD8, 0xD8, 0xD8, 0xD8, 0xD8, 0xD8, // C
    0xD8, 0x08, 0xE8, 0xE8, 0x0A, 0x0A, 0x0A, 0x0A, 0xE8, 0xE8, 0xE8, 0xE8, 0xE8, 0xE8, 0xE8, 0xE8, // D
    0xFD, 0xFD, 0xF8, 0xF8, 0x0A, 0x0A, 0x0A, 0x0A, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, 0xF8, // E
    0xDD, 0x4D, 0xE0, 0xE0, 0x0A, 0x0A, 0x0A, 0x0A, 0x88, 0x88, 0x08, 0x08, 0x88, 0x88, 0x08, 0x08, // F
};

// Opt-in diagnostic — set POM2_DEBUG_DISK=1 to see one-shot lines marking
// each milestone of the Disk II boot sequence. Cheap when off (one bool
// check on hot-path reads, but only inside the diagnostic state struct's
// guard).
bool debugEnabled()
{
    static const bool on = []{
        const char* v = std::getenv("POM2_DEBUG_DISK");
        return v && *v && *v != '0';
    }();
    return on;
}

}  // namespace

DiskIICard::DiskIICard()
{
    bootRom.fill(0xFF);
    // Pre-fill with the published 16-sector P6 PROM so the LSS path
    // works out-of-the-box. `loadLssRom()` can override this with a
    // verified dump from `roms/diskii_p6.rom`. `p6RomLoaded` stays
    // false until an explicit load — when false, `useBitLss` also
    // stays false (legacy gate path) so cycle accounting and test
    // expectations match the pre-LSS behaviour.
    std::memcpy(p6Rom.data(), kP6RomDefault, sizeof(kP6RomDefault));
}

bool DiskIICard::loadBootRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pom2::log().warn("Disk II", "Cannot open boot PROM: " + path);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != 256) {
        pom2::log().warn("Disk II",
            "Boot PROM must be 256 bytes, got " + std::to_string(size));
        return false;
    }
    f.read(reinterpret_cast<char*>(bootRom.data()), 256);
    if (!f) {
        pom2::log().warn("Disk II", "Short read on " + path);
        return false;
    }
    bootRomLoaded = true;
    pom2::log().info("Disk II", "Boot PROM loaded: " + path);
    return true;
}

bool DiskIICard::loadLssRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pom2::log().info("Disk II",
            "P6 LSS PROM not found at " + path +
            " — using legacy 32-cycle gate");
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != 256) {
        pom2::log().warn("Disk II",
            "P6 LSS PROM must be 256 bytes, got " + std::to_string(size) +
            " — using legacy 32-cycle gate");
        return false;
    }
    f.read(reinterpret_cast<char*>(p6Rom.data()), 256);
    if (!f) {
        pom2::log().warn("Disk II", "Short read on " + path);
        return false;
    }
    p6RomLoaded = true;
    pom2::log().info("Disk II", "P6 LSS PROM loaded: " + path);
    return true;
}

bool DiskIICard::insertDisk(const std::string& path)
{
    // Save any pending writes from the previous image before we drop it.
    if (image.isLoaded() && image.hasUnsavedChanges()) {
        if (!image.saveDirty()) {
            pom2::log().warn("Disk II",
                "Save-on-swap failed: " + image.getLastError());
        }
    }
    if (!image.loadFile(path)) {
        pom2::log().warn("Disk II", "Insert failed: " + image.getLastError());
        return false;
    }
    image.setWriteBackEnabled(writeBackEnabled);
    trackPos    = 0;
    cycleAccum  = 0;
    writeLatch  = 0xFF;
    // Reset LSS bit-stream cursor + state. The default P6 ROM is in
    // p6Rom (see constructor) so the LSS path works without the
    // optional roms/diskii_p6.rom override. We still gate on
    // `p6RomLoaded` so the user can opt out by removing the file.
    bitPos           = 0;
    subCellTick      = 0;
    lssState         = 0;
    lssData          = 0;
    writeShifter     = 0;
    writeShifterBits = 0;
    useBitLss        = p6RomLoaded;   // active iff explicit override loaded
    return true;
}

void DiskIICard::ejectDisk()
{
    if (image.isLoaded() && image.hasUnsavedChanges()) {
        if (!image.saveDirty()) {
            pom2::log().warn("Disk II",
                "Save-on-eject failed: " + image.getLastError());
        }
    }
    image.eject();
    trackPos = 0;
    writeLatch = 0xFF;
}

uint8_t DiskIICard::slotRomRead(uint8_t low8)
{
    if (debugEnabled() && !trace.sawSlotRom) {
        trace.sawSlotRom = true;
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "PROM entered (first slot ROM read at offset $%02X)", low8);
        pom2::log().info("Disk II", buf);
    }
    return bootRom[low8];
}

void DiskIICard::onReset()
{
    motorOn   = false;
    writeMode = false;
    loadMode  = false;
    phaseOn   = {};
    cycleAccum = 0;
    dataLatch  = 0;
    byteReady  = false;
    // LSS state. Real Disk II hardware does NOT lose the head's bit-
    // stream alignment on a soft (Ctrl-Reset) — the disk keeps spinning
    // and the LSS shift register settles back to a stable value within
    // the next sync gap. We zero the state/data registers to start from
    // a clean PROM state; bitPos / subCellTick survive (the PROM re-
    // syncs naturally on the next sync run).
    lssState         = 0;
    lssData          = 0;
    writeShifter     = 0;
    writeShifterBits = 0;
    trace = {};
    // Head position and trackPos are intentionally NOT reset — a real
    // Disk II loses neither when the host CPU is reset; the boot PROM
    // re-recalibrates by stepping phase 0 a bunch of times anyway.
}

void DiskIICard::onPhaseEdge(int /*phase*/, bool turningOn)
{
    // Step the head ±1 half-track (= 2 qt) per rising edge, with the
    // direction chosen by which adjacent phase magnet is currently on.
    // Identical to the prior half-track algorithm, just expressed in
    // quarter-track storage so future LSS-driven sub-half-track stepping
    // (used by some copy-protected disks) can layer on top without
    // changing this rule.
    if (!turningOn) return;
    int dir = 0;
    const int cur = (headQuarterTrack / 2) & 3;     // half-track mod 4
    if (phaseOn[(cur + 1) & 3]) dir += 1;
    if (phaseOn[(cur + 3) & 3]) dir -= 1;
    headQuarterTrack = std::clamp(headQuarterTrack + 2 * dir, 0, kQuarterTrackMax);
}

void DiskIICard::advanceCycles(int cycles)
{
    if (!motorOn || !image.isLoaded()) return;
    if (!useBitLss) { legacyAdvance(cycles); return; }

    // Real hardware: 2 LSS ticks per CPU cycle (LSS clock = 2.04 MHz,
    // CPU clock = 1.02 MHz). Each LSS tick runs one PROM lookup +
    // ALU op on the data register and consumes 1/8 of a bit cell.
    int ticks = cycles * 2;
    while (ticks-- > 0) lssTick();
}

void DiskIICard::legacyAdvance(int cycles)
{
    cycleAccum += cycles;
    while (cycleAccum >= kCyclesPerNibble) {
        cycleAccum -= kCyclesPerNibble;
        trackPos  = (trackPos + 1) % DiskImage::kNibblesPerTrack;
        if (writeMode) {
            // Write the most-recently-latched nibble onto the track.
            // Real hardware streams bits through the LSS shift register;
            // we model one nibble per ~32 CPU cycles, matching the read
            // pacing. The CPU latches the next nibble via a soft-switch
            // store (see deviceSelectWrite).
            image.writeNibbleAt(headQuarterTrack / 4, trackPos, writeLatch);
            ++writeFlushCount;
        } else {
            dataLatch = image.nibbleAt(headQuarterTrack / 4, trackPos);
            byteReady = true;
        }
    }
}

// One LSS tick = 0.5 µs = 1/8 of a bit cell. Port of the apple2js
// reference implementation (`WozDiskDriver.ts moveHead`), which is in
// turn a transcription of MAME `wozfdc.cpp lss_sync()`. The P6 PROM is
// indexed by `(state << 4) | (Q7 << 3) | (Q6 << 2) | (QA << 1) |
// (!PULSE)` where QA is the data register MSB; the byte at that address
// is the opcode whose high nibble is the next state and whose low nibble
// is the ALU op on the data register.
void DiskIICard::lssTick()
{
    if (!image.isLoaded()) return;

    // 1. Sample the current bit-cell window. Each cell = 8 LSS ticks.
    //    A "1" cell is a single PULSE asserted at the *middle* of the
    //    cell (sub-tick 4 of 8); other sub-ticks see PULSE=0.
    const int track = headQuarterTrack / 4;
    const int len   = image.trackBitLength(track);
    int pulse = 0;
    if (subCellTick == 4 && len > 0) {
        pulse = image.bitAt(track, bitPos) ? 1 : 0;
    }

    // 2. Form the 8-bit PROM address.
    const uint8_t idx = static_cast<uint8_t>(
          (pulse ? 0x00 : 0x01)                      // bit 0: !PULSE
        | (((lssData & 0x80) != 0) ? 0x02 : 0x00)    // bit 1: QA
        | (loadMode  ? 0x04 : 0x00)                  // bit 2: Q6
        | (writeMode ? 0x08 : 0x00)                  // bit 3: Q7
        | static_cast<uint8_t>(lssState << 4)        // bits 4..7: state
    );

    const uint8_t op = p6Rom[idx];

    // 3. Dispatch low nibble = ALU op on data register.
    //    0x0 CLR  data ← 0
    //    0x8 NOP
    //    0x9 SL0  data ← (data << 1)
    //    0xA SR   data ← (data >> 1) | (write-protect ? 0x80 : 0)
    //    0xB LD   data ← writeLatch (CPU-supplied byte)
    //    0xD SL1  data ← (data << 1) | 1
    switch (op & 0x0F) {
        case 0x0: lssData = 0;                                             break;
        case 0x8:                                                          break;
        case 0x9: lssData = static_cast<uint8_t>(lssData << 1);            break;
        case 0xA: lssData = static_cast<uint8_t>((lssData >> 1)
                  | (image.isWriteProtected() ? 0x80 : 0x00));             break;
        case 0xB: lssData = writeLatch;                                    break;
        case 0xD: lssData = static_cast<uint8_t>((lssData << 1) | 0x01);   break;
        default:                                                           break;
    }

    // 4. Latch new state for the next tick. Opcode high nibble is the
    //    next-state value directly; bit 3 of state is also the WRITE_DATA
    //    bit driven onto the disk surface in write mode.
    lssState = static_cast<uint8_t>((op >> 4) & 0x0F);

    // 5. Advance the bit-cell counter. At each cell boundary (sub-tick
    //    transitioning from 7 → 0): if Q7 (write mode) is on, shift the
    //    write bit (state bit 3) into writeShifter. After 8 bits, write
    //    the assembled nibble into the track buffer at trackPos and
    //    step trackPos. This preserves the 32-CPU-cycle nibble cadence
    //    the legacy gate used (8 cells × 8 LSS ticks / 2 ticks-per-cyc
    //    = 32 cyc) so existing write tests still pass.
    if (++subCellTick == 8) {
        subCellTick = 0;
        if (writeMode) {
            const bool writeBit = (lssState & 0x08) != 0;
            writeShifter = static_cast<uint8_t>((writeShifter << 1)
                | (writeBit ? 1 : 0));
            if (++writeShifterBits == 8) {
                if (writeBackEnabled) {
                    image.writeNibbleAt(track, trackPos, writeShifter);
                    ++writeFlushCount;
                }
                trackPos = (trackPos + 1) % DiskImage::kNibblesPerTrack;
                writeShifterBits = 0;
            }
        }
        if (len > 0) bitPos = (bitPos + 1) % len;
    }
}

void DiskIICard::handleSwitchAccess(uint8_t low4)
{
    if (debugEnabled() && !trace.sawDevSelect) {
        trace.sawDevSelect = true;
        char buf[64];
        std::snprintf(buf, sizeof(buf),
            "first soft switch hit (low4=$%X)", low4);
        pom2::log().info("Disk II", buf);
    }

    if (low4 < 8) {
        const int  phase = low4 >> 1;
        const bool on    = (low4 & 1) != 0;
        const bool was   = phaseOn[phase];
        phaseOn[phase]   = on;
        if (on && !was) onPhaseEdge(phase, true);
        return;
    }
    switch (low4) {
        case 0x8: motorOn = false; break;
        case 0x9:
            if (!motorOn) {
                cycleAccum = 0;
                if (debugEnabled() && !trace.sawMotorOn) {
                    trace.sawMotorOn = true;
                    char buf[96];
                    std::snprintf(buf, sizeof(buf),
                        "motor ON (head at quarter-track %d, disk %s)",
                        headQuarterTrack,
                        image.isLoaded() ? "loaded" : "MISSING");
                    pom2::log().info("Disk II", buf);
                }
            }
            motorOn = true;
            break;
        case 0xA: /* drive 1 select — only drive modelled */ break;
        case 0xB: /* drive 2 select — silently ignored      */ break;
        case 0xC: loadMode  = false; break;     // Q6L
        case 0xD: loadMode  = true;  break;     // Q6H
        case 0xE: writeMode = false; break;     // Q7L
        case 0xF: writeMode = true;  break;     // Q7H (no-op for read-only)
        default: break;
    }
}

uint8_t DiskIICard::deviceSelectRead(uint8_t low4)
{
    handleSwitchAccess(low4);

    // $C0nC in (Q6 low, Q7 low) = read mode → return current track nibble.
    // The DOS 3.3 fast read loop is `LDA $C0EC ; BPL loop` — every valid
    // GCR nibble has bit-7 set, so the BPL terminates immediately.
    if (low4 == 0xC && !writeMode && !loadMode) {
        if (!image.isLoaded() || !motorOn) return 0xFF;
        if (useBitLss) {
            // MAME's `read_data_register` ticks the LSS once *before*
            // sampling the data register — this models the real read-
            // pipe latency where the latched value the CPU sees was
            // valid one LSS clock ago. Without this, address marks
            // arrive one bit early in the BPL spin.
            lssTick();
            if (debugEnabled() && (lssData & 0x80) && !trace.sawFirstNibble) {
                trace.sawFirstNibble = true;
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                    "first GCR nibble served: $%02X (LSS, track %d, bit %d)",
                    lssData, headQuarterTrack / 4, bitPos);
                pom2::log().info("Disk II", buf);
            }
            return lssData;
        }
        // Legacy gate. Bit-7 of the data register goes high only after a
        // full nibble has shifted in. While `byteReady` is false, the PROM
        // sees a "still assembling" value — bit-7 clear keeps the BPL spin
        // running until advanceCycles latches the next nibble. After one
        // successful read the latch stays valid (real hardware doesn't
        // clear it on read), but `byteReady` drops back to false so the
        // next LDA waits the full 32-cycle window.
        const uint8_t out = byteReady ? dataLatch : (dataLatch & 0x7F);
        if (debugEnabled() && byteReady && !trace.sawFirstNibble) {
            trace.sawFirstNibble = true;
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                "first GCR nibble served: $%02X (track %d, pos %d)",
                dataLatch, headQuarterTrack / 4, trackPos);
            pom2::log().info("Disk II", buf);
        }
        byteReady = false;
        return out;
    }
    // $C0nD in (Q6 high, Q7 low) = write-protect probe. Bit-7 set means
    // protected; reflects the user's write-back opt-in. When write-back
    // is OFF (default) we return protected, so DOS will SAVE-error
    // before scrambling the in-memory nibble buffer. Once the user opts
    // in, the disk reports writable and updates land back in the file.
    if (low4 == 0xD && !writeMode) {
        if (!image.isLoaded()) return 0x00;
        return image.isWriteProtected() ? 0x80 : 0x00;
    }
    return 0;
}

void DiskIICard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Apple II soft switches respond to writes too — DOS 3.3 occasionally
    // pokes them via STA. We only need the side-effect, not the value
    // for control switches; for $C0nD in WRITE mode (Q6H + Q7H) the store
    // loads the LSS data latch. $C0nC stores are SHIFT strobes on real
    // hardware (Sather "Understanding the Apple II" 9-13) — they do not
    // load the latch register.
    handleSwitchAccess(low4);
    if (writeMode && low4 == 0xD) {
        writeLatch = v;
        // Realign the LSS to the CPU's store cadence. Real Disk II
        // hardware reloads its WRITE shift register from the data latch
        // at the start of every 8-bit cell; if the CPU's store-to-store
        // interval drifts a few cycles past 32 (DOS RWTS WRITE6 has
        // small gaps between the data prologue and the first secondary
        // nibble — bookkeeping, not part of the inner write loop), our
        // free-running cycleAccum would overflow and flush a duplicate
        // of the still-stale latch into the next nibble slot. That
        // extra nibble shifts the running-XOR checksum and DOS reports
        // I/O ERROR. Resetting cycleAccum here pins each flush to
        // exactly 32 cycles after the most recent latch load — which
        // matches DOS's intent and AppleWin's per-access LSS sync
        // (Disk_t::DataLatchWriteWOZ → UpdateBitStreamOffsets).
        cycleAccum = 0;
    }
}

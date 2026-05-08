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

// Stepper "magnetic well" model (Apple Disk II hardware behaviour).
//
// Each of the 4 phase magnets attracts the head toward a quarter-track
// position whose index mod 4 equals the phase number. With multiple
// magnets energized, the head settles at the resultant of their pulls:
//   - one magnet  → its own well  (single attractor)
//   - two adjacent magnets → between them (e.g. phases 0+1 → ½ qt)
//   - two opposing magnets (0+2 or 1+3) → forces cancel, head holds
//   - three magnets → centroid of the three wells
//   - all four (or none) → no net pull, head holds
//
// Indexed by the 4-bit phaseOn bitmask (bit i = phase i). Each entry is
// the desired qt offset modulo 4, or -1 when there is no clear target
// (head should not move). This 16-entry table is derived from the spec
// described in Sather's "Understanding the Apple II", chap. 9; the same
// pattern is reproduced in MAME's apple2 floppy device. Adjacent-pair
// rows snap toward the lower phase number, except 0+3 which wraps and
// snaps to phase 3.
constexpr int kPhaseTarget[16] = {
    /* 0000 none           */ -1,
    /* 0001 phase 0        */  0,
    /* 0010 phase 1        */  1,
    /* 0011 phases 0+1     */  0,
    /* 0100 phase 2        */  2,
    /* 0101 phases 0+2 opp */ -1,
    /* 0110 phases 1+2     */  1,
    /* 0111 phases 0+1+2   */  1,
    /* 1000 phase 3        */  3,
    /* 1001 phases 0+3     */  3,
    /* 1010 phases 1+3 opp */ -1,
    /* 1011 phases 0+1+3   */  0,
    /* 1100 phases 2+3     */  2,
    /* 1101 phases 0+2+3   */  2,
    /* 1110 phases 1+2+3   */  2,
    /* 1111 all            */ -1,
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
    trackPos   = 0;
    cycleAccum = 0;
    writeLatch = 0xFF;
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
        // LSS contract: bit-7 of the data register goes high only after a
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

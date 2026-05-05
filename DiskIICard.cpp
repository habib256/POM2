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
constexpr int kCyclesPerNibble = 32;
constexpr int kHalfTrackMax    = (DiskImage::kTracks - 1) * 2;   // 68

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
    if (!image.loadFile(path)) {
        pom2::log().warn("Disk II", "Insert failed: " + image.getLastError());
        return false;
    }
    trackPos   = 0;
    cycleAccum = 0;
    return true;
}

void DiskIICard::ejectDisk()
{
    image.eject();
    trackPos = 0;
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
    // Standard rule: the head moves toward whichever neighbour magnet is
    // currently energized. Examining the on/off state of the two phases
    // adjacent to our half-track (mod 4) gives the step direction.
    // Only rising edges actually pull the head; a phase-off transition
    // releases the magnet but doesn't move the head on its own.
    if (!turningOn) return;
    int dir = 0;
    const int cur = headHalfTrack & 3;
    if (phaseOn[(cur + 1) & 3]) dir += 1;
    if (phaseOn[(cur + 3) & 3]) dir -= 1;
    headHalfTrack = std::clamp(headHalfTrack + dir, 0, kHalfTrackMax);
}

void DiskIICard::advanceCycles(int cycles)
{
    if (!motorOn || !image.isLoaded()) return;
    cycleAccum += cycles;
    while (cycleAccum >= kCyclesPerNibble) {
        cycleAccum -= kCyclesPerNibble;
        trackPos  = (trackPos + 1) % DiskImage::kNibblesPerTrack;
        dataLatch = image.nibbleAt(headHalfTrack / 2, trackPos);
        byteReady = true;     // a fresh nibble is now in the data register
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
                        "motor ON (head at half-track %d, disk %s)",
                        headHalfTrack,
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
                dataLatch, headHalfTrack / 2, trackPos);
            pom2::log().info("Disk II", buf);
        }
        byteReady = false;
        return out;
    }
    // $C0nD in (Q6 high, Q7 low) = write-protect probe. Bit-7 set means
    // protected; we ship read-only so always assert it when media is in.
    if (low4 == 0xD && !writeMode) {
        return image.isLoaded() ? 0x80 : 0x00;
    }
    return 0;
}

void DiskIICard::deviceSelectWrite(uint8_t low4, uint8_t /*v*/)
{
    // Apple II soft switches respond to writes too — DOS 3.3 occasionally
    // pokes them via STA. We only need the side-effect, not the value.
    handleSwitchAccess(low4);
}

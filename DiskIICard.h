// POM2 Apple II Emulator
// Copyright (C) 2026
//
// DiskIICard — Apple Disk II Interface card (slot 6 by convention).
// Read-only first cut: a single drive, 16-sector DOS 3.3 (.dsk / .do)
// images, no nibble write-back.
//
// Soft-switch map (slot N at $C080+N*16; for slot 6 → $C0E0-$C0EF):
//
//   $C0n0/n1   Phase 0 off / on    head stepper coil 0
//   $C0n2/n3   Phase 1 off / on
//   $C0n4/n5   Phase 2 off / on
//   $C0n6/n7   Phase 3 off / on
//   $C0n8      Drive (motor) off
//   $C0n9      Drive (motor) on
//   $C0nA      Select drive 1
//   $C0nB      Select drive 2
//   $C0nC      Q6L  — shift / read data register
//   $C0nD      Q6H  — load / write-protect probe
//   $C0nE      Q7L  — read mode
//   $C0nF      Q7H  — write mode (we acknowledge but never alter media)
//
// Slot ROM ($Cs00-$CsFF, s=6 → $C600-$C6FF) is the Apple-Disk-II "P5A"
// 256-byte boot PROM. The PROM autodetects its slot via the standard
// JSR-$FF58 / TSX / LDA $0100,X trick, then steps the head to track 0,
// reads the first sector via the address-field/data-field state machine,
// and JMPs to the loaded boot1 at $0801.
//
// Head stepping: real Disk II hardware pulls the head magnet coils in a
// 4-phase rotational pattern. The head's mechanical position is at any
// quarter-track offset; each phase magnet has a "well" at a quarter-track
// position whose index (mod 4) matches the phase number. With one magnet
// energized the head settles at that magnet's well; with two adjacent
// magnets energized the head settles between them; opposing magnets
// (0+2 or 1+3) cancel and the head holds. State is held in
// *quarter-tracks* (0..139 = 35 tracks × 4) so disks with quarter-track
// copy protection step accurately. The same algorithm is what MAME's
// `apple2_floppy_image_device` uses (see its phase-to-target lookup).
//
// Timing: at 1.0227 MHz with 4 µs bit cells, the LSS shift register
// outputs one nibble every ~32 CPU cycles. We accumulate cycles via
// advanceCycles() and step the track-buffer cursor accordingly. Reads of
// $C0nC return the current nibble; bit-7 is implicitly always set
// because every valid GCR nibble has it set.

#ifndef POM2_DISK_II_CARD_H
#define POM2_DISK_II_CARD_H

#include "DiskImage.h"
#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

class DiskIICard : public SlotPeripheral
{
public:
    DiskIICard();

    /// Force the head back to track 0 and reset the LSS state. Used by the
    /// "Boot disk" UI shortcut so the boot PROM finds D5 AA 96 quickly even
    /// if the head wandered while waiting for a disk insert.
    void seekTrack0() { headQuarterTrack = 0; trackPos = 0; cycleAccum = 0; }

    /// Load the 256-byte Disk II boot PROM from disk. Must succeed before
    /// the card is useful — without the PROM, $C600-$C6FF reads back
    /// $FF and `PR#6` jumps into nothing.
    bool loadBootRom(const std::string& path);
    bool hasBootRom() const { return bootRomLoaded; }

    /// Insert / eject a .dsk image in drive 1 (the only drive modelled).
    bool insertDisk(const std::string& path);
    void ejectDisk();

    bool isDiskLoaded() const { return image.isLoaded(); }
    const std::string& getDiskPath()  const { return image.getPath(); }
    const std::string& getLastError() const { return image.getLastError(); }

    int  getCurrentTrack() const { return headQuarterTrack / 4; }
    int  getHalfTrack()    const { return headQuarterTrack / 2; }
    int  getQuarterTrack() const { return headQuarterTrack; }
    bool isMotorOn()       const { return motorOn; }
    int  getTrackPosition() const { return trackPos; }
    bool hasUnsavedChanges() const { return image.hasUnsavedChanges(); }
    /// Total nibble write flushes since last reset. Used by the
    /// dos33_save smoke test to confirm SAVE actually exercised the
    /// write pipeline (vs. erroring out before any write).
    uint64_t getWriteFlushCount() const { return writeFlushCount; }

    /// User opt-in for write-back. When true, eject (and explicit save)
    /// rewrites the source file with any modified sectors. Default off
    /// to avoid silently mutating the user's image file.
    void setWriteBackEnabled(bool on) { image.setWriteBackEnabled(on); writeBackEnabled = on; }
    bool isWriteBackEnabled() const   { return writeBackEnabled; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Disk II"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;

private:
    DiskImage image;
    std::array<uint8_t, 256> bootRom{};
    bool bootRomLoaded = false;

    bool motorOn   = false;
    bool writeMode = false;     // Q7 latch: false=read, true=write
    bool loadMode  = false;     // Q6 latch: false=shift, true=load
    bool writeBackEnabled = false;     // forwarded to DiskImage on toggle
    uint8_t writeLatch = 0xFF;         // latched data nibble for next bit-cell flush

    // Head stepper. phaseOn[i] = magnet i currently energized.
    std::array<bool, 4> phaseOn{};
    // Head position in quarter-tracks. 35 tracks × 4 qt = 140; the head
    // can sit at any qt from 0 (track 0) to 4*(kTracks-1) = 136 (track 34).
    // Quarter-tracks are needed for some copy protections; the standard
    // DOS 3.3 / ProDOS skew uses whole tracks (qt mod 4 == 0).
    int headQuarterTrack = 0;

    // Position into the current track's nibble buffer (0..6655). Wraps
    // continuously while the motor is on.
    int trackPos   = 0;
    int cycleAccum = 0;       // CPU cycles since the last nibble advance

    // LSS shift-register model. While a new nibble is assembling, the
    // data register's bit 7 reads as 0 (intermediate shift state), so
    // the host CPU's `LDA $C0EC ; BPL loop` busy-wait holds until the
    // full byte is in. Without this, the PROM's "match D5 then read AA"
    // sequence sees the same nibble twice and loops forever.
    uint8_t dataLatch = 0;
    bool    byteReady = false;

    uint64_t writeFlushCount = 0;

    void handleSwitchAccess(uint8_t low4);
    void onPhaseEdge(int phase, bool turningOn);

    // Boot-trace one-shot flags (POM2_DEBUG_DISK=1). Reset at onReset().
    struct {
        bool sawSlotRom     = false;
        bool sawDevSelect   = false;
        bool sawMotorOn     = false;
        bool sawFirstNibble = false;
    } trace;
};

#endif // POM2_DISK_II_CARD_H

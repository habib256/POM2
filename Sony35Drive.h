// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Sony35Drive — 3.5" Sony floppy drive model attached to the IWM. Port
// of MAME's `applefdintf_device::add_35` device (the Apple "Sony 3.5"
// 800 KB drive used in the Macintosh 128 / Mac Plus / Apple //c+ /
// Apple //gs).
//
// The 3.5" drive's interface to the IWM is **fundamentally different**
// from the Disk II 5.25" drive's stepper-motor wiring:
//
//   * On 5.25", IWM phase lines φ0..φ3 are connected to four solenoids
//     that step the head one quarter-track per pulse pattern.
//
//   * On 3.5" Sony, the same four phase pins become a tiny command bus:
//     CA0, CA1, CA2(=PH2/HD), LSTRB(=PH3). The IWM also exposes SEL
//     (drive select) — together they address an 8-line "register"
//     space inside the drive.
//
//       Read register select  = { SEL, CA2, CA1, CA0 }
//       Write register select = same bits, latched by an LSTRB pulse
//
//     Reads come back on the SENSE line, which the IWM samples on
//     `read_register` (control = $40) reads. Writes are toggles —
//     pulsing LSTRB high with a given register address performs a
//     drive action (motor on, motor off, eject, step, direction set).
//
//     Documented in *Inside the Apple //gs* (hardware reference) and
//     mirrored in MAME `src/devices/imagedev/mac_floppy.cpp`. The
//     register table below lists the bits POM2 currently honours.
//
// Phase 1 scope (this file):
//   * `seekPhaseW(uint8_t phases)` — interprets the phase bits as a
//     read-register select OR a strobed write-register, drives the
//     internal state (motor on/off, eject, direction, head select).
//   * `senseR()` — returns the current SENSE bit for the selected
//     read register. The //c+ firmware probes the drive via this
//     register set during the cold-boot dispatch (`$C8xx` SmartPort
//     handler in bank 1) — without this, the probe reads back garbage
//     and the firmware loops indefinitely.
//   * `monW(bool stop)` — motor-enable / monitor wire-OR'd from the
//     IWM. Tracks the floppy spin state for SENSE reads.
//   * Disk image attach (`setImage(Disk35Image*)`).
//
// Phase 2 (deferred, separate session):
//   * `nextTransition(int from)` — return the next flux event timestamp.
//     For 3.5" this requires the zoned Sony GCR encoder
//     (`src/lib/formats/ap_dsk35.cpp`) to convert the loaded block
//     payload into a flux stream the IWM walker can clock out.
//   * `writeFlux(start, end, count, transitions)` — splice IWM-emitted
//     flux into the loaded image (write-back path).

#ifndef POM2_SONY35_DRIVE_H
#define POM2_SONY35_DRIVE_H

#include <cstdint>

namespace pom2 {

class Disk35Image;

class Sony35Drive
{
public:
    Sony35Drive();

    /// Power-on / cold-reset state. Motor off, no disk-change pending,
    /// direction = inward, track 0, head 0.
    void reset();

    /// Attach (or detach with nullptr) the Disk35Image slot the drive
    /// reads from. The slot is a stable container owned by the host;
    /// loading/ejecting media inside that container is signalled by
    /// `notifyMediaChange()` (drives the disk-change flip-flop).
    void setImage(Disk35Image* image);
    Disk35Image* image() const { return image_; }

    /// Pulse the disk-change flip-flop. Call after Disk35Image::loadFile
    /// or eject() so the //c+ firmware's "media changed" SmartPort
    /// probe latches the new state.
    void notifyMediaChange();

    /// True iff a disk is currently inserted (slot wired AND image
    /// loaded inside it). Out-of-line to avoid pulling Disk35Image's
    /// full definition into every includer of this header.
    bool isInserted() const;

    /// IWM motor-enable line (m_floppy->mon_w in MAME). When the IWM
    /// enters MODE_ACTIVE it pulls this low (motor on); MODE_DELAY /
    /// MODE_IDLE pull it high (motor off).
    void monW(bool motorOffHigh);

    /// IWM head-select wire (m_floppy->ss_w). MIG routes this from the
    /// 3.5" $C240/$C260 toggles in the bank-1 expansion ROM area. Side
    /// 0 = false, side 1 = true.
    void ssW(bool side1);

    /// IWM phase bus (m_floppy->seek_phase_w). Bits 0..3 = CA0, CA1,
    /// CA2 (or PH2 / HD), LSTRB. SEL is a separate IWM control bit
    /// `m_control & 0x20` → device select 1 vs 2; we don't have it
    /// here so we feed it through `setSel`.
    void seekPhaseW(uint8_t phases);

    /// IWM SEL wire — bit 5 of IWM control register. Distinguishes
    /// "external" vs "internal" drive on the //c+ but also doubles as
    /// the high bit of the 3.5" register select.
    void setSel(bool sel);

    /// SENSE line readout — the IWM samples this when the host reads
    /// the IWM status register (control = $40). MAME returns it via
    /// `m_floppy->wpt_r()` on the IWM read path.
    /// Returns true if SENSE is HIGH (=> IWM read-side reports WP bit
    /// SET, i.e. the high bit of the status byte is 1).
    bool senseR() const;

    /// Convenience accessors for inspectors / save state.
    bool isMotorOn()        const { return motorOn_; }
    bool isWriteProtected() const { return writeProtect_; }
    int  track()            const { return track_; }
    bool side1()            const { return side1_; }

private:
    Disk35Image* image_         = nullptr;
    bool         motorOn_       = false;
    bool         writeProtect_  = true;   // safe default until image probed
    bool         side1_         = false;
    bool         sel_           = false;
    bool         directionIn_   = true;   // true → step toward track 0
    bool         diskSwitched_  = false;  // disk-change flip-flop
    int          track_         = 0;
    uint8_t      phases_        = 0;
    uint8_t      prevPhases_    = 0;

    /// 4-bit register address: { SEL, CA2, CA1, CA0 } as documented in
    /// *Inside the Apple //gs* Sec. 6 "Sony Drive". Read addresses are
    /// the same bit pattern as writes but sampled in real time.
    uint8_t regSelect() const;

    /// Strobe a write-register address (called on LSTRB rising edge).
    void strobeWriteRegister(uint8_t reg);
};

}  // namespace pom2

#endif // POM2_SONY35_DRIVE_H

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Port of MAME `src/devices/machine/iwm.cpp` (`iwm_device`).
// Line refs throughout point back at the MAME source so future
// updates can be diff-checked. The MAME source-of-truth header is
// `/tmp/iwm.txt` in this checkout (a snapshot fetched via scrapling).
//
// Adaptations from MAME:
//   * `attotime` / `machine().time()` → POM2's CPU cycle counter
//     (`uint64_t`, advanced by the caller via `tick(nowCycles)`).
//   * `emu_timer` → POM2 has no timer subsystem, so the 1-emu-second
//     drive-disable delay (MAME `iwm.cpp:83-97 update_timer_tick`)
//     runs inline from `sync()` when the recorded `delayDeadline_`
//     is reached. EmulationController pulses `tick()` every video
//     frame so the deadline still fires when no $C0Ex traffic
//     arrives between operations.
//   * `floppy_image_device::get_next_transition(attotime)` →
//     `DiskImage::getNextTransition(qt, fromLssCycle)` for 5.25",
//     `Sony35Drive::nextTransition` for 3.5".
//   * `floppy_image_device::write_flux(start, end, count, transitions)` →
//     `DiskImage::writeFlux` / `Sony35Drive::writeFlux`.
//   * `m_devsel_cb` (host notifies drive select) → `devselCb_` fired
//     by `fireDevsel()` at MAME-faithful times: reset(0), MODE_DELAY
//     entry (1 or 2 before the timer), MODE_DELAY drain (0), and on
//     steady-state SEL transitions while active.
//   * `m_floppy->mon_w(motorOff)` → `notifyMonW(motorOff)`. For 5.25"
//     this is a no-op (DiskIICard owns motor + spin-down + audio for
//     the Disk II path). For 3.5" it fires `Sony35Drive::monW` so
//     the Sony stack's motorOn_ tracks the IWM as the master.
//
// Intentionally divergent (POM2-only):
//   * `read()` doesn't gate `control()` on `machine().side_effects_
//     disabled()` because POM2's debug surface (Memory viewer) reads
//     RAM directly and never goes through soft switches → no caller
//     needs a side-effect-free $C0Ex read today.
//   * Window sizes are scaled from MAME's IWM-clock ticks to POM2's
//     CPU-clock ticks (÷7 since //c+ runs the IWM off A2BUS_7M while
//     POM2 keeps a single cycle counter). See `windowSize` /
//     `halfWindowSize` / `readRegisterUpdateDelay` for the rounding
//     choices.
//
// Not yet ported (groundwork for a follow-up pass):
//   * `applefdintf_device::device_start/reset` base members.
//   * The Q3 fast clock (1.86 MHz) used on Mac/IIgs but not //c+.
//   * Full `set_write_splice` handling — the call site fires but
//     `DiskImage::setWriteSplice` is still a stub (TODO.md «WOZ1
//     splice point (TRK +6650) ignoré»).

#include "IWMDevice.h"
#include "CpuClock.h"
#include "Logger.h"
#include "Sony35Drive.h"

#include <algorithm>
#include <climits>
#include <cstdio>

namespace {

// MAME `iwm.cpp:204-206`: 1 << 23 = 8388608 ticks of the IWM's clock
// (which is the CPU clock for //c / //c+ — no separate Q3 clock), so
// "≈ 1 emulated second" is the design intent. POM2 ticks IWM time at
// the CPU clock too, so 1 second = POM2_CPU_CLOCK_HZ cycles.
constexpr uint64_t kDriveDisableDelayCycles =
    static_cast<uint64_t>(POM2_CPU_CLOCK_HZ);

}

namespace pom2 {

IWMDevice::IWMDevice()
{
    reset();
}

void IWMDevice::reset()
{
    // MAME `iwm.cpp:59-81` device_reset.
    lastSync_         = now_;
    nextStateChange_  = 0;
    active_           = MODE_IDLE;
    rw_               = MODE_IDLE;
    rwState_          = S_IDLE;
    data_             = 0x00;
    whd_              = 0xBF;
    mode_             = 0x00;
    status_           = 0x00;
    control_          = 0x00;
    wsh_              = 0x00;
    rsh_              = 0x00;
    fluxWriteStart_   = 0;
    fluxWriteCount_   = 0;
    rwBitCount_       = 0;
    phases_           = 0;
    writeDataLoaded_  = false;
    q3ClockActive_    = false;
    syncUpdate_       = 0;
    asyncUpdate_      = 0;
    // MAME `iwm.cpp:78-79` fires `m_devsel_cb(0)` once during reset
    // so the host hub can drop any latched device-select state. POM2
    // mirrors via `fireDevsel(0)` (idempotent if already 0).
    fireDevsel(0);
}

void IWMDevice::fireDevsel(uint8_t value)
{
    if (devsel_ != value) {
        devsel_ = value;
        if (devselCb_) devselCb_(devsel_);
    }
}

void IWMDevice::notifyMonW(bool motorOff)
{
    if (sony_) {
        sony_->monW(motorOff);
    }
    // 5.25" DiskImage path: DiskIICard's motor + spin-down + audio
    // wiring is the source of truth (Theme 6 audit). The IWM here is
    // a shadow on //c+ (read path authoritative when iwmAuthoritative
    // is set), so we intentionally don't propagate mon_w to the disk
    // image — DiskIICard fires its own FloppySoundSink::motor on the
    // same $C0E8/$C0E9 access that drove this controlAccess() call.
}

void IWMDevice::setFloppy(DiskImage* disk, int qt)
{
    // MAME `iwm.cpp:99-115 set_floppy`. When the active floppy changes
    // while the motor is enabled, MAME drops mon_w on the OLD drive
    // then raises mon_w on the NEW drive — i.e. the new drive sees the
    // motor come on as part of the rebind. POM2 mirrors this for the
    // 3.5" Sony path (DiskIICard owns 5.25" motor sound).
    if (disk_ == disk && qt_ == qt && !sony_) return;
    sync(now_);
    flushWrite();
    const bool motorOn = (control_ & 0x10) != 0;
    if (motorOn) notifyMonW(true);     // stop old (if any 3.5" was active)
    disk_ = disk;
    qt_   = qt;
    sony_ = nullptr;                   // routing back to 5.25" path
    // No mon_w(false) here — the 5.25" Disk II path uses DiskIICard's
    // motor wiring exclusively.
}

void IWMDevice::setSony35(Sony35Drive* drive)
{
    if (sony_ == drive && !disk_) return;
    sync(now_);
    flushWrite();
    const bool motorOn = (control_ & 0x10) != 0;
    if (motorOn) notifyMonW(true);     // stop old drive's motor (Sony if any)
    sony_ = drive;
    disk_ = nullptr;
    if (sony_) {
        // Anchor the revolution: the freshly-attached drive's cell 0
        // is under the head at "now". MAME re-anchors on `mon_w(false)`
        // (motor spin-up); POM2 collapses both events to the moment
        // the SmartPort hub points the IWM at this drive.
        revStart35_ = now_;
        sony_->invalidateCache();
        if (motorOn) notifyMonW(false); // raise mon_w on new drive
    }
}

uint8_t IWMDevice::read(uint8_t offset)
{
    // MAME `iwm.cpp:103-114 read`. The `!machine().side_effects_disabled()`
    // guard there protects debugger peeks; POM2 doesn't expose a
    // side-effect-disabled read yet, so we always run `control()`.
    controlAccess(offset & 0xF, 0x00);
    switch (control_ & 0xC0) {
        case 0x00: return active_ ? data_ : 0xFF;
        case 0x40: {
            // (status & 0x7F) | wpt. MAME `iwm.cpp:107` reads
            // `m_floppy->wpt_r()`. For 3.5" Sony drives this is the
            // SENSE line on the currently-selected register (see
            // Sony35Drive::senseR comment block) — the //c+ firmware
            // probes /WPT, /TRACK0, /INSERTED, etc. via this same bit.
            bool wpt;
            if (sony_)      wpt = sony_->senseR();
            else if (disk_) wpt = disk_->isWriteProtected();
            else            wpt = true;
            return static_cast<uint8_t>((status_ & 0x7F) | (wpt ? 0x80 : 0x00));
        }
        case 0x80: return whd_;
        case 0xC0: return 0xFF;
    }
    return 0xFF;     // unreachable
}

void IWMDevice::write(uint8_t offset, uint8_t data)
{
    // MAME `iwm.cpp:115-118 write`.
    controlAccess(offset & 0xF, data);
}

void IWMDevice::flushWrite(uint64_t when)
{
    // MAME `iwm.cpp:119-143 flush_write`. Slice the buffered transitions
    // into the backing disk image, leaving the bit-cell write splice
    // pinned at the next event.
    if (!fluxWriteStart_) return;
    if (!when) when = lastSync_;
    if (when > fluxWriteStart_) {
        bool lastOnEdge = (fluxWriteCount_ > 0 &&
                           fluxWrite_[fluxWriteCount_ - 1] == when);
        if (lastOnEdge) --fluxWriteCount_;
        // The flux transition values in MAME live in `attotime`; POM2
        // already speaks raw cycle counts at the DiskImage boundary, so
        // we pass them through as-is.
        if (disk_) {
            std::vector<int64_t> fluxes;
            fluxes.reserve(fluxWriteCount_);
            for (uint32_t i = 0; i < fluxWriteCount_; ++i) {
                fluxes.push_back(static_cast<int64_t>(fluxWrite_[i]));
            }
            disk_->writeFlux(qt_,
                             static_cast<int64_t>(fluxWriteStart_),
                             static_cast<int64_t>(when),
                             static_cast<int>(fluxes.size()),
                             fluxes.empty() ? nullptr : fluxes.data());
        }
        if (sony_) {
            // 3.5" Sony write-back. Sony35Drive's writeFlux splices
            // the new transitions into its cached cell stream, then
            // runs MAME's `extract_sectors_from_track_mac_gcr6`
            // decoder over the result and pushes any complete
            // sector's 512-byte payload back into the attached
            // Disk35Image. Write protection is enforced inside
            // writeFlux (early-return on `isWriteProtected()`).
            std::vector<int64_t> fluxes;
            fluxes.reserve(fluxWriteCount_);
            for (uint32_t i = 0; i < fluxWriteCount_; ++i) {
                fluxes.push_back(static_cast<int64_t>(fluxWrite_[i]));
            }
            sony_->writeFlux(static_cast<int64_t>(fluxWriteStart_),
                             static_cast<int64_t>(when),
                             fluxes.empty() ? nullptr : fluxes.data(),
                             static_cast<int>(fluxes.size()),
                             static_cast<int64_t>(revStart35_));
        }
        fluxWriteCount_ = 0;
        if (lastOnEdge) {
            fluxWrite_[fluxWriteCount_++] = when;
        }
        fluxWriteStart_ = when;
    } else {
        fluxWriteCount_ = 0;
    }
}

void IWMDevice::controlAccess(int offset, uint8_t data)
{
    // MAME `iwm.cpp:144-254 control`. Long function — split into:
    //   1. Phase 0-3 vs control-bit update (low 8 vs 8-15 offsets)
    //   2. Active state transition (motor-enable bit 4 of m_control)
    //   3. Read/write transition (Q7 bit 7 of m_control)
    //   4. Async-update scheduling for read-side timing
    //   5. mode_w / data_w dispatch on Q7H+Q6H+odd offset writes
    //
    // The phases (low 4 offsets) drive the head stepper on a real
    // Disk II. POM2's DiskIICard already owns the head — we keep the
    // raw bit tracking here so the IWM's `m_phases` mirrors MAME, but
    // don't re-invoke the stepper here (DiskIICard does it).

    sync(now_);
    if (offset < 8) {
        // Phases (offset 0..7 → phase bits 0..3, even=clear odd=set).
        // POM2's DiskIICard handles the 5.25" head-stepper effect of
        // these; in addition, we forward the 4-bit pattern to the
        // host-installed `phasesCb_` so a Sony 3.5" drive (the //c+
        // SmartPort target) can interpret them as a CA0/CA1/CA2/LSTRB
        // command bus. MAME `iwm.cpp:147-152` updates `m_phases` then
        // calls `update_phases()` which in turn fires `phases_cb`.
        const uint8_t bit = static_cast<uint8_t>(1u << ((offset >> 1) & 3));
        const uint8_t prev = phases_;
        if (offset & 1) phases_ |=  bit;
        else            phases_ &= ~bit;
        if (phases_ != prev && phasesCb_) phasesCb_(phases_);
        (void)data;
    } else {
        const uint8_t prevControl = control_;
        if (offset & 1) control_ |=  (1u << ((offset >> 1) & 7));
        else            control_ &= ~(1u << ((offset >> 1) & 7));
        // SEL (bit 5) is exposed to the host so 3.5" drives can fold
        // it into their register-select bus on the next phase strobe.
        // MAME `iwm.cpp` doesn't expose SEL via a dedicated callback —
        // it's read by `m_floppy` on every `seek_phase_w` from the
        // host's `update_phases()`. POM2 mirrors the same effect by
        // notifying `phasesCb_` whenever SEL transitions, so the
        // active 3.5" drive can re-evaluate its sense address.
        if (((prevControl ^ control_) & 0x20) && phasesCb_) {
            phasesCb_(phases_);
        }
    }

    // Activate / deactivate based on m_control bit 4 (motor enable).
    // MAME line 190-241.
    if (control_ & 0x10) {
        if (active_ != MODE_ACTIVE) {
            active_      = MODE_ACTIVE;
            status_     |= 0x20;
            // MAME `iwm.cpp:194-195`: `m_floppy->mon_w(false)`. For 5.25"
            // DiskIICard fires the motor sample on its own $C0E9 path
            // and stays source of truth; the 3.5" Sony drive needs the
            // signal forwarded so its motorOn_ tracks the IWM rather
            // than depending on strobe-register motor commands alone.
            notifyMonW(false);
        }
        if ((control_ & 0x80) == 0x00) {
            // Q7 = 0 → read mode
            if (rw_ != MODE_READ) {
                if (rw_ == MODE_WRITE) {
                    flushWrite();
                    writeClockStop();
                }
                rw_              = MODE_READ;
                rwState_         = S_IDLE;
                nextStateChange_ = 0;
                syncUpdate_      = 0;
                asyncUpdate_     = 0;
                data_            = 0x00;
            }
        } else {
            // Q7 = 1 → write mode
            if (rw_ != MODE_WRITE) {
                rw_              = MODE_WRITE;
                rwState_         = S_IDLE;
                whd_            |= 0x40;
                nextStateChange_ = 0;
                writeDataLoaded_ = false;
                writeClockStart();
                // MAME `iwm.cpp:218-221`:
                //   m_floppy->set_write_splice(
                //       cycles_to_time(m_flux_write_start));
                // The splice position pins the bit cell where a WOZ
                // re-master should start writing — Applesauce uses it
                // to keep round-trip parity. POM2's DiskImage exposes
                // a stub `setWriteSplice` (DiskImage.h:219); see
                // TODO.md «WOZ1 splice point (TRK +6650) ignoré» for
                // the full plumbing. Call site is here so the day the
                // stub gets a body, the splice arrives at the right
                // moment automatically. No-op on Sony35Drive — 3.5"
                // images don't carry a splice position.
                if (disk_) {
                    disk_->setWriteSplice(qt_,
                        static_cast<int64_t>(fluxWriteStart_));
                }
            }
        }
    } else {
        if (active_ == MODE_ACTIVE) {
            flushWrite();
            if (mode_ & 0x04) {
                // Timer mode: drop immediately to idle (MAME line
                // 226-234). `m_floppy->mon_w(true)` is fired here.
                writeClockStop();
                active_   = MODE_IDLE;
                rw_       = MODE_IDLE;
                rwState_  = S_IDLE;
                status_  &= ~0x20;
                whd_     &= ~0x40;
                notifyMonW(true);
            } else {
                // Normal mode: 1 emulated-second drive-disable delay.
                // MAME `iwm.cpp:235-239` schedules an emu_timer for
                // `cycles_to_time(8388608)` and fires `m_devsel_cb`
                // with the current drive number BEFORE the timer
                // fires (so the host hub can pre-emptively recalc the
                // active device while the motor coasts to a stop).
                // POM2 records a deadline; `sync()` checks it on
                // entry and runs the drain when reached. The drive-
                // disable fire of `m_floppy->mon_w(true)` happens
                // when the timer expires (see sync()), NOT here.
                fireDevsel(static_cast<uint8_t>(
                    (control_ & 0x20) ? 2 : 1));
                active_         = MODE_DELAY;
                delayDeadline_  = now_ + kDriveDisableDelayCycles;
            }
        }
    }

    // Steady-state devsel update (MAME line 243-247). Captures motor-
    // active drive-select transitions that aren't covered by the
    // MODE_DELAY-entry fire above (e.g. SEL bit flip while still
    // spinning).
    const uint8_t newDevsel = (active_ != MODE_IDLE)
        ? ((control_ & 0x20) ? 2 : 1)
        : 0;
    fireDevsel(newDevsel);

    // Read-side state reset (MAME line 214-215).
    if ((control_ & 0xC0) == 0x40 &&
        active_ == MODE_ACTIVE && rw_ == MODE_READ) {
        rsh_ = 0;
    }

    // Asynchronous mode update scheduling (MAME line 246-247). MAME's `14`
    // is in IWM-clock ticks (= half a default 28-tick window); POM2 runs the
    // FSM on the CPU clock and scales every window constant by /7 (see
    // windowSize / halfWindowSize / readRegisterUpdateDelay). This one was
    // left raw — 14/7 = 2 — so the async "data goes stale → 0" timer fired
    // ~7× late on 3.5" async reads.
    if (active_ && !(control_ & 0x80) && !isSync() && (data_ & 0x80)) {
        asyncUpdate_ = lastSync_ + 2;
    }

    // Mode register / data register write (MAME line 248-254).
    if ((control_ & 0xC0) == 0xC0 && (offset & 1)) {
        if (active_) dataW(data);
        else         modeW(data);
    }
}

void IWMDevice::modeW(uint8_t data)
{
    // MAME `iwm.cpp:256-269 mode_w`.
    mode_   = data;
    status_ = static_cast<uint8_t>((status_ & 0xE0) | (data & 0x1F));
}

void IWMDevice::dataW(uint8_t data)
{
    // MAME `iwm.cpp:311-318 data_w`. Three side effects in order:
    //   1. Always latch the data byte (visible via $C0nF read once the
    //      controller is back in MODE_IDLE).
    //   2. In sync write mode, mirror the byte into the write shift
    //      register IMMEDIATELY (the FSM also copies data_ into wsh_ at
    //      SW_WINDOW_LOAD, but a CPU write that lands between two cells
    //      should be seen by the next bit-out without an extra round
    //      trip — MAME parity).
    //   3. If "latched handshake" mode is selected (mode bit 0 = 1),
    //      clear WHD bit 7 to signal "data loaded". When that mode bit
    //      is 0, the IWM does NOT auto-clear the handshake — the CPU is
    //      expected to use a different write-pacing protocol. (This is
    //      the gate POM2 originally got wrong: it cleared whd bit 7 on
    //      every sync+write data_w regardless of mode bit 0, which
    //      ignored the mode register entirely.)
    data_ = data;
    if (isSync() && rw_ == MODE_WRITE) {
        wsh_ = data;
    }
    if (mode_ & 0x01) {
        whd_ &= 0x7F;
        writeDataLoaded_ = true;
    }
}

// MAME's IWM window sizes are in IWM-clock ticks (the //c / //c+ runs
// the IWM off A2BUS_7M ≈ 7.16 MHz — see `apple2e.cpp` machine config).
// POM2 ticks the IWM with the CPU clock (POM2_CPU_CLOCK_HZ ≈ 1.023
// MHz) to keep one cycle counter for the whole machine. Scale the
// MAME constants by the clock ratio (≈ 7) so a "bit cell" window
// still spans ≈ 4 µs of emulated time, which is what GCR-encoded
// 5.25" flux transitions assume. The constants below preserve MAME's
// relative ratios across the four mode-bit-4-3 combinations.

uint64_t IWMDevice::halfWindowSize() const
{
    // MAME `iwm.cpp:290-301 half_window_size`, scaled CPU-clock units.
    if (q3ClockActive_) {
        return (mode_ & 0x08) ? 2 : 4;
    }
    switch (mode_ & 0x18) {
        case 0x00: return 2;     // MAME 14  / 7
        case 0x08: return 1;     // MAME 7   / 7
        case 0x10: return 2;     // MAME 16  / 7 (rounded down)
        case 0x18: return 1;     // MAME 8   / 7 (rounded down)
    }
    return 2;
}

uint64_t IWMDevice::windowSize() const
{
    // MAME `iwm.cpp:302-313 window_size`, scaled CPU-clock units.
    if (q3ClockActive_) {
        return (mode_ & 0x08) ? 4 : 8;
    }
    switch (mode_ & 0x18) {
        case 0x00: return 4;     // MAME 28  / 7
        case 0x08: return 2;     // MAME 14  / 7
        case 0x10: return 5;     // MAME 36  / 7 (rounded)
        case 0x18: return 2;     // MAME 16  / 7 (rounded down)
    }
    return 4;
}

uint64_t IWMDevice::readRegisterUpdateDelay() const
{
    // MAME `iwm.cpp:363-366`: 4 IWM ticks when mode bit 3 is set,
    // 8 otherwise. The IWM ticks at ~7.16 MHz on a //c+ while POM2
    // runs everything off `POM2_CPU_CLOCK_HZ` (~1.02 MHz), so the
    // raw ratio is 1/7. Round-up (ceil) the two values:
    //   4/7 = 0.57 → 1 CPU cycle
    //   8/7 = 1.14 → 2 CPU cycles
    // Round-down would collapse both branches to 1 and erase the
    // mode-bit distinction, which while sub-CPU-cycle in absolute
    // terms is still meaningful for relative ordering of register
    // updates (sync mode polls the data register against this
    // delay). Round-up at least preserves the mode-bit signal.
    return (mode_ & 0x08) ? 1 : 2;
}

void IWMDevice::writeClockStart()
{
    // MAME `iwm.cpp:318-326`. With Q3 inactive on //c+, just records
    // the splice cycle.
    if (isSync() && q3Clock_) {
        q3ClockActive_ = true;
        lastSync_      = now_;
    }
    fluxWriteStart_ = lastSync_;
    fluxWriteCount_ = 0;
}

void IWMDevice::writeClockStop()
{
    // MAME `iwm.cpp:327-334`.
    if (q3ClockActive_) {
        q3ClockActive_ = false;
        lastSync_      = now_;
    }
    fluxWriteStart_ = 0;
}

int64_t IWMDevice::nextTransition(int64_t from) const
{
    // Dispatch by floppy form factor:
    //  * 3.5" Sony: query Sony35Drive directly. Its flux events are
    //    already expressed in CPU cycles (the Sony zoned-recording
    //    keeps the bit-cell rate constant at ~505 kHz so no LSS
    //    half-cycle scaling is needed).
    //  * 5.25" Disk II: DiskImage's flux events live in LSS-cycle
    //    space (= 2× CPU cycles) — see DiskIICard `lssCycle =
    //    cpuCycleTotal * 2`. We transit the boundary here.
    if (sony_) {
        const int64_t t = sony_->nextTransition(from, static_cast<int64_t>(revStart35_));
        if (t != INT64_MAX) return t;
        return noiseTransition(from);
    }
    if (disk_) {
        const int64_t fromLss = from * 2;
        const int64_t t = disk_->getNextTransition(qt_, fromLss);
        if (t != DiskImage::kFluxNever) return t / 2;
    }
    return noiseTransition(from);
}

int64_t IWMDevice::noiseTransition(int64_t from) const
{
    // No media (or an unformatted track) — but the head is parked over a
    // spinning, empty drive. A real Apple drive feeds the IWM a stream of
    // noise flux in this state, so the read shift register keeps assembling
    // garbage bytes with bit-7 ("byte ready") set. The boot firmware relies
    // on that: its wait-for-byte loop ($C0EC bit 7) must keep advancing so
    // the per-read retry counter can drain and the machine can fall through
    // to its "no disk" path (//c "Check Disk Drive", //c+ "UNABLE TO FIND A
    // BOOTABLE DISK ONLINE."). If we instead returned INT64_MAX the FSM only
    // ever shifts in 0-bits, bit-7 never asserts, and the firmware spins
    // forever on the un-cleared power-up screen. MAME models this implicitly:
    // the floppy reports no transition but the IWM's window timer still
    // cycles the SR; POM2 collapsed that timer into nextTransition(), so the
    // garbage has to be injected here.
    const uint64_t w = windowSize() ? windowSize() : 1;
    // Deterministic LCG keyed on the window index: reproducible for tests,
    // yet straddles window boundaries so the SR accumulates a mix of 1s and
    // 0s (rsh_ reaches 0x80 within a few windows -> byte ready).
    uint64_t s = static_cast<uint64_t>(from) / w;
    s = s * 6364136223846793005ULL + 1442695040888963407ULL;
    const int64_t span = static_cast<int64_t>(w + 1 + ((s >> 33) % (2 * w + 1)));
    return from + span;
}

void IWMDevice::sync(uint64_t nowCycles)
{
    // MAME `iwm.cpp:335-480 sync`. Verbatim port of the read/write
    // state-machine walker. The read side rebuilds the SR data byte
    // by sliding a window over flux transitions from the disk image;
    // the write side drains the CPU's loaded data byte into the IWM
    // shift register, scheduling flux events into `fluxWrite_`.

    // Drive-disable delay drain (MAME `iwm.cpp:83-97 update_timer_tick`).
    // The MAME implementation runs this from an emu_timer at the
    // scheduled deadline; POM2 has no timer system so we check the
    // deadline at every sync() entry instead. Idempotent: zeroing
    // `delayDeadline_` after the transition stops it from re-firing.
    //
    // Side effects (in MAME order, line 86-95):
    //   * flush_write — drain any pending flux events
    //   * m_active = MODE_IDLE; m_rw = MODE_IDLE; m_rw_state = S_IDLE
    //   * m_floppy->mon_w(true)  — motor off, real on the 3.5" Sony
    //                              path; 5.25" Disk II spin-down is
    //                              owned by DiskIICard (see notifyMonW)
    //   * m_devsel_cb(0); m_devsel = 0
    //   * m_status &= ~0x20; m_whd &= ~0x40
    if (active_ == MODE_DELAY &&
        delayDeadline_ != 0 &&
        nowCycles >= delayDeadline_) {
        flushWrite();
        active_     = MODE_IDLE;
        rw_         = MODE_IDLE;
        rwState_    = S_IDLE;
        notifyMonW(true);
        fireDevsel(0);
        status_    &= ~0x20;
        whd_       &= ~0x40;
        delayDeadline_ = 0;
    }
    if (!active_) return;
    const uint64_t nextSync = nowCycles;
    switch (rw_) {
        case MODE_IDLE:
            lastSync_ = nextSync;
            break;

        case MODE_READ: {
            int64_t nextFluxChange = 0;
            while (nextSync > lastSync_) {
                if (nextFluxChange <= static_cast<int64_t>(lastSync_)) {
                    nextFluxChange = nextTransition(static_cast<int64_t>(lastSync_ + 1));
                    if (nextFluxChange <= static_cast<int64_t>(lastSync_)) {
                        nextFluxChange = static_cast<int64_t>(lastSync_ + 1);
                    }
                }
                if (nextSync < nextStateChange_) {
                    lastSync_ = nextSync;
                    break;
                }
                if (lastSync_ < nextStateChange_) {
                    lastSync_ = nextStateChange_;
                }
                switch (rwState_) {
                    case S_IDLE:
                        rsh_              = 0x00;
                        rwState_          = SR_WINDOW_EDGE_0;
                        nextStateChange_  = lastSync_ + windowSize();
                        syncUpdate_       = 0;
                        asyncUpdate_      = 0;
                        break;
                    case SR_WINDOW_EDGE_0:
                    case SR_WINDOW_EDGE_1: {
                        const uint64_t endw = nextStateChange_ +
                            (rwState_ == SR_WINDOW_EDGE_0 ? windowSize() : halfWindowSize());
                        if (rwState_ == SR_WINDOW_EDGE_0 &&
                            static_cast<int64_t>(endw) >= nextFluxChange &&
                            static_cast<int64_t>(nextSync) >= nextFluxChange) {
                            lastSync_        = static_cast<uint64_t>(nextFluxChange);
                            nextStateChange_ = lastSync_;
                            rwState_         = SR_WINDOW_EDGE_1;
                            break;
                        }
                        if (nextSync < endw) {
                            lastSync_ = nextSync;
                            break;
                        }
                        rsh_ = static_cast<uint8_t>(
                            (rsh_ << 1) | (rwState_ == SR_WINDOW_EDGE_1 ? 1 : 0));
                        nextStateChange_ = lastSync_ = endw;
                        rwState_         = SR_WINDOW_EDGE_0;
                        if (isSync()) {
                            if (rsh_ >= 0x80) {
                                data_ = rsh_;
                                rsh_  = 0;
                            } else if (rsh_ >= 0x04) {
                                data_       = rsh_;
                                syncUpdate_ = 0;
                            } else if (rsh_ >= 0x02) {
                                syncUpdate_ = lastSync_ + readRegisterUpdateDelay();
                            }
                        } else if (rsh_ >= 0x80) {
                            data_         = rsh_;
                            asyncUpdate_  = 0;
                            rsh_          = 0;
                        }
                        break;
                    }
                }
            }
            if (syncUpdate_ && syncUpdate_ <= lastSync_) {
                if (isSync()) data_ = rsh_;
                syncUpdate_ = 0;
            }
            if (asyncUpdate_ && asyncUpdate_ <= lastSync_) {
                if (!isSync()) data_ = 0;
                asyncUpdate_ = 0;
            }
            break;
        }

        case MODE_WRITE: {
            while (nextSync > lastSync_) {
                if (nextSync < nextStateChange_ || !(whd_ & 0x40)) {
                    lastSync_ = nextSync;
                    break;
                }
                if (lastSync_ < nextStateChange_) {
                    lastSync_ = nextStateChange_;
                }
                switch (rwState_) {
                    case S_IDLE:
                        fluxWriteCount_ = 0;
                        if (mode_ & 0x02) {
                            rwState_         = SW_WINDOW_LOAD;
                            rwBitCount_      = 8;
                            nextStateChange_ = lastSync_ + 1;  // MAME 7 / 7
                        } else {
                            wsh_             = data_;
                            rwState_         = SW_WINDOW_MIDDLE;
                            nextStateChange_ = lastSync_ + halfWindowSize();
                        }
                        break;
                    case SW_WINDOW_LOAD:
                        if (whd_ & 0x80) {
                            // Underrun — CPU didn't load next byte in
                            // time. Only warn if the CPU had actually
                            // started a write sequence (≥1 dataW since
                            // entering MODE_WRITE); the spurious case
                            // where firmware probes Q7=1 with no
                            // intent to write would otherwise fire
                            // this every boot/media-change because
                            // whd_'s cold value (0xBF) has bit 7 set.
                            if (writeDataLoaded_) {
                                pom2::log().warn("IWM", "write underrun");
                            }
                            flushWrite(nextSync);
                            writeClockStop();
                            whd_      &= ~0x40;
                            lastSync_  = nextSync;
                            rwState_   = SW_UNDERRUN;
                        } else {
                            wsh_             = data_;
                            rwState_         = SW_WINDOW_MIDDLE;
                            whd_            |= 0x80;
                            // MAME `half_window_size() - 7`; both terms are in
                            // IWM ticks. POM2 scaled halfWindowSize() by /7 but
                            // left the 7 raw → halfWindowSize()∈{1,2} - 7
                            // underflowed in uint64_t. 7/7 = 1.
                            nextStateChange_ = lastSync_ + halfWindowSize() - 1;
                        }
                        break;
                    case SW_WINDOW_MIDDLE:
                        if (wsh_ & 0x80) {
                            if (fluxWriteCount_ < fluxWrite_.size()) {
                                fluxWrite_[fluxWriteCount_++] = lastSync_;
                            }
                        }
                        wsh_             = static_cast<uint8_t>(wsh_ << 1);
                        rwState_         = SW_WINDOW_END;
                        nextStateChange_ = lastSync_ + halfWindowSize();
                        break;
                    case SW_WINDOW_END:
                        if (fluxWriteCount_ == fluxWrite_.size()) {
                            flushWrite();
                        }
                        if (mode_ & 0x02) {
                            --rwBitCount_;
                            if (rwBitCount_ == 0) {
                                rwState_         = SW_WINDOW_LOAD;
                                rwBitCount_      = 8;
                                nextStateChange_ = lastSync_ + 1;  // MAME 7 / 7
                            } else {
                                rwState_         = SW_WINDOW_MIDDLE;
                                nextStateChange_ = lastSync_ + halfWindowSize();
                            }
                        } else {
                            nextStateChange_ = lastSync_ + halfWindowSize();
                            rwState_         = SW_WINDOW_MIDDLE;
                        }
                        break;
                    case SW_UNDERRUN:
                        lastSync_ = nextSync;
                        break;
                }
            }
            break;
        }
    }
}

}  // namespace pom2

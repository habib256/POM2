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
//   * `emu_timer` → no equivalent today. The MAME timer drains the
//     drive-disable delay (MAME `iwm.cpp:70-84`). POM2 currently runs
//     that drain inline from `sync()` once enough time has elapsed —
//     adequate for nibble-rate boot but would miss the long-tail
//     0xBF transition on a real drive spin-down. The corresponding
//     comment is tagged "TODO timer" so the next pass knows.
//   * `floppy_image_device::get_next_transition(attotime)` →
//     `DiskImage::getNextTransition(qt, fromLssCycle)`.
//   * `floppy_image_device::write_flux(start, end, count, transitions)` →
//     `DiskImage::writeFlux(qt, start, end, count, &transitions[0])`.
//   * `m_devsel_cb` (host notifies drive select) → callback omitted;
//     POM2's slot-6 multiplex is currently DiskIICard-internal.
//
// Not yet ported (groundwork for a follow-up pass):
//   * `applefdintf_device::device_start/reset` base members.
//   * `set_floppy` mon_w / set_write_splice plumbing (POM2 currently
//     drives those from DiskIICard; the IWM would take ownership).
//   * The Q3 fast clock (1.86 MHz) used on Mac/IIgs but not //c+.

#include "IWMDevice.h"
#include "CpuClock.h"
#include "Logger.h"

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
    // MAME `iwm.cpp:48-69` device_reset.
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
    devsel_           = 0;
    phases_           = 0;
    q3ClockActive_    = false;
    syncUpdate_       = 0;
    asyncUpdate_      = 0;
}

void IWMDevice::setFloppy(DiskImage* disk, int qt)
{
    // MAME `iwm.cpp:85-98 set_floppy`. We don't bind a `mon_w` callback
    // here yet (the LSS card still owns motor sound + writeback gating).
    if (disk_ == disk && qt_ == qt) return;
    sync(now_);
    flushWrite();
    disk_ = disk;
    qt_   = qt;
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
            // (status & 0x7F) | wpt
            const bool wpt = (!disk_) || disk_->isWriteProtected();
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
    // MAME line 159-208.
    if (control_ & 0x10) {
        if (active_ != MODE_ACTIVE) {
            active_      = MODE_ACTIVE;
            status_     |= 0x20;
            // m_floppy->mon_w(false) — POM2 wires the motor sound via
            // DiskIICard; nothing to do here.
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
                writeClockStart();
                // set_write_splice — DiskImage already pins splice from
                // setWriteSplice; left as a no-op match for parity.
            }
        }
    } else {
        if (active_ == MODE_ACTIVE) {
            flushWrite();
            if (mode_ & 0x04) {
                // Timer mode: drop immediately to idle.
                writeClockStop();
                active_   = MODE_IDLE;
                rw_       = MODE_IDLE;
                rwState_  = S_IDLE;
                status_  &= ~0x20;
                whd_     &= ~0x40;
                // m_floppy->mon_w(true) — handled by DiskIICard.
            } else {
                // Normal mode: 1 emulated-second drive-disable delay.
                // MAME `iwm.cpp:202-206` schedules an emu_timer for
                // `cycles_to_time(8388608)` then runs `update_timer_tick`
                // when it fires. POM2 records a deadline; `sync()`
                // checks it on entry and runs the same drain when
                // reached. EmulationController also pulses `tick()`
                // each frame so the deadline still fires when no
                // $C0Ex traffic arrives between operations.
                active_         = MODE_DELAY;
                delayDeadline_  = now_ + kDriveDisableDelayCycles;
            }
        }
    }

    // Devsel update (MAME line 209-213). POM2 now fires the optional
    // host callback so the SmartPort hub can call
    // `recalc_active_device` (MAME `apple2e.cpp:638-679`).
    const uint8_t newDevsel = (active_ != MODE_IDLE)
        ? ((control_ & 0x20) ? 2 : 1)
        : 0;
    if (newDevsel != devsel_) {
        devsel_ = newDevsel;
        if (devselCb_) devselCb_(devsel_);
    }

    // Read-side state reset (MAME line 214-215).
    if ((control_ & 0xC0) == 0x40 &&
        active_ == MODE_ACTIVE && rw_ == MODE_READ) {
        rsh_ = 0;
    }

    // Asynchronous mode update scheduling (MAME line 246-247).
    if (active_ && !(control_ & 0x80) && !isSync() && (data_ & 0x80)) {
        asyncUpdate_ = lastSync_ + 14;
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
    // MAME `iwm.cpp:270-275 data_w`.
    data_ = data;
    if (isSync() && rw_ == MODE_WRITE) {
        whd_ &= ~0x80;
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
    // MAME `iwm.cpp:314-317`, scaled CPU-clock units.
    return (mode_ & 0x08) ? 1 : 1;
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
    // DiskImage's flux events live in LSS-cycle space (= 2× CPU cycles)
    // — see DiskIICard `lssCycle = cpuCycleTotal * 2`. The IWM state
    // machine in this port runs at CPU cycle resolution to stay
    // single-clock with the rest of POM2, so we transit the boundary
    // here: scale `from` up before the lookup, and the returned
    // transition stamp back down to CPU cycles.
    if (!disk_) return INT64_MAX;
    const int64_t fromLss = from * 2;
    const int64_t t = disk_->getNextTransition(qt_, fromLss);
    if (t == DiskImage::kFluxNever) return INT64_MAX;
    return t / 2;
}

void IWMDevice::sync(uint64_t nowCycles)
{
    // MAME `iwm.cpp:335-480 sync`. Verbatim port of the read/write
    // state-machine walker. The read side rebuilds the SR data byte
    // by sliding a window over flux transitions from the disk image;
    // the write side drains the CPU's loaded data byte into the IWM
    // shift register, scheduling flux events into `fluxWrite_`.

    // Drive-disable delay drain (MAME `iwm.cpp:70-84 update_timer_tick`).
    // The MAME implementation runs this from an emu_timer at the
    // scheduled deadline; POM2 has no timer system so we check the
    // deadline at every sync() entry instead. Idempotent: zeroing
    // `delayDeadline_` after the transition stops it from re-firing.
    if (active_ == MODE_DELAY &&
        delayDeadline_ != 0 &&
        nowCycles >= delayDeadline_) {
        flushWrite();
        active_     = MODE_IDLE;
        rw_         = MODE_IDLE;
        rwState_    = S_IDLE;
        devsel_     = 0;
        status_    &= ~0x20;
        whd_       &= ~0x40;
        delayDeadline_ = 0;
        // MAME also calls `m_floppy->mon_w(true)` and notifies the
        // devsel callback here. POM2's audio + writeback hook lives
        // on DiskIICard; the same $C0E8 (motor-off) access that put
        // us in MODE_DELAY already started DiskIICard's spin-down
        // delay, which fires its own `sound_->motor(false, ...)` from
        // `advance()`. The two timers run in lock-step (both bound
        // to `POM2_CPU_CLOCK_HZ`), so no cross-callback is needed.
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
                            nextStateChange_ = lastSync_ + 7;
                        } else {
                            wsh_             = data_;
                            rwState_         = SW_WINDOW_MIDDLE;
                            nextStateChange_ = lastSync_ + halfWindowSize();
                        }
                        break;
                    case SW_WINDOW_LOAD:
                        if (whd_ & 0x80) {
                            // Underrun — CPU didn't load next byte in time.
                            pom2::log().warn("IWM", "write underrun");
                            flushWrite(nextSync);
                            writeClockStop();
                            whd_      &= ~0x40;
                            lastSync_  = nextSync;
                            rwState_   = SW_UNDERRUN;
                        } else {
                            wsh_             = data_;
                            rwState_         = SW_WINDOW_MIDDLE;
                            whd_            |= 0x80;
                            nextStateChange_ = lastSync_ + halfWindowSize() - 7;
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
                                nextStateChange_ = lastSync_ + 7;
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

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Port of MAME's Apple 3.5" Sony drive model (`applefdintf_device::add_35`
// + `floppy_image_device` SENSE / phase decoder). The "phases-as-command"
// protocol is documented in *Inside the Apple //gs* hardware reference
// chapter 6 ("Sony Drive"); MAME's authoritative source is in
// `src/devices/imagedev/mac_floppy.cpp::seek_phase_w` and
// `::wpt_r`. Where MAME would consult the loaded `floppy_image` for
// per-track state (write-protect tab, disk-in-drive switch, etc.), POM2
// queries the attached `Disk35Image`.
//
// Register table (read & write both indexed by { SEL, CA2, CA1, CA0 }):
//
//   addr  bits SEL CA2 CA1 CA0   read SENSE        write strobe (LSTRB pulse)
//   ----  ------------------------------------------------------------
//   0x0    0   0   0   0         /DIRTN            (direction set inward)
//   0x1    0   0   0   1         /STEP-in-progress (single step)
//   0x2    0   0   1   0         /MOTOR-ON         motor on
//   0x3    0   0   1   1         /TRACK0           motor off
//   0x4    0   1   0   0         /SWITCHED         (eject — Sony only)
//   0x5    0   1   0   1         (reserved)        (reserved)
//   0x6    0   1   1   0         /TACH             (reserved)
//   0x7    0   1   1   1         (reserved)        (reserved)
//   0x8    1   0   0   0         /SIDES            (set head 0)
//   0x9    1   0   0   1         (reserved)        (set head 1)
//   0xA    1   0   1   0         /READY            (reserved)
//   0xB    1   0   1   1         /INSERTED         (reserved)
//   0xC    1   1   0   0         (reserved)        (reserved)
//   0xD    1   1   0   1         /SEL              (reserved)
//   0xE    1   1   1   0         (reserved)        (reserved)
//   0xF    1   1   1   1         /DRVIN            (reserved)
//
// Apple's protocol uses ACTIVE-LOW logic on most read lines (the "/"
// prefix), so a "1" returned to the IWM means the named condition is
// NOT present. We expose this via `senseR()` returning the *raw* bit
// (true = HIGH = inactive).

#include "Sony35Drive.h"
#include "Disk35Image.h"
#include "Logger.h"

#include <string>

namespace pom2 {

namespace {

constexpr uint8_t kBitCA0   = 0x01;
constexpr uint8_t kBitCA1   = 0x02;
constexpr uint8_t kBitCA2   = 0x04;
constexpr uint8_t kBitLSTRB = 0x08;

}  // namespace

Sony35Drive::Sony35Drive()
{
    reset();
}

bool Sony35Drive::isInserted() const
{
    return image_ && image_->isLoaded();
}

void Sony35Drive::reset()
{
    motorOn_       = false;
    writeProtect_  = true;
    side1_         = false;
    sel_           = false;
    directionIn_   = true;
    diskSwitched_  = false;
    track_         = 0;
    phases_        = 0;
    prevPhases_    = 0;
    if (image_) {
        writeProtect_ = image_->isWriteProtected();
    }
}

void Sony35Drive::setImage(Disk35Image* image)
{
    // image_ is the drive *slot* — a stable Disk35Image instance owned
    // by EmulationController. Mounting / ejecting media is a separate
    // event signalled via `notifyMediaChange()`.
    image_ = image;
    writeProtect_ = image && image->isWriteProtected();
}

void Sony35Drive::notifyMediaChange()
{
    diskSwitched_ = true;
    writeProtect_ = image_ && image_->isWriteProtected();
    pom2::log().info(
        "Sony35",
        std::string("media change ") +
            ((image_ && image_->isLoaded()) ? image_->path() : "(empty)"));
}

void Sony35Drive::monW(bool motorOffHigh)
{
    // MAME: m_floppy->mon_w(true) = motor STOP. The IWM calls this when
    // it leaves MODE_ACTIVE.
    motorOn_ = !motorOffHigh;
}

void Sony35Drive::ssW(bool side1)
{
    side1_ = side1;
}

void Sony35Drive::setSel(bool sel)
{
    sel_ = sel;
}

uint8_t Sony35Drive::regSelect() const
{
    // { SEL, CA2, CA1, CA0 }
    uint8_t r = (phases_ & (kBitCA2 | kBitCA1 | kBitCA0));
    if (sel_) r |= 0x08;
    return r;
}

void Sony35Drive::seekPhaseW(uint8_t phases)
{
    // MAME `mac_floppy.cpp::seek_phase_w`: latch the new phase bits,
    // then if LSTRB transitioned 0→1 fire `strobeWriteRegister` with
    // the current `regSelect()` address. The IWM is free to change
    // CA0/CA1/CA2 while LSTRB is held — MAME only fires the strobe on
    // the rising edge.
    prevPhases_ = phases_;
    phases_     = static_cast<uint8_t>(phases & 0x0F);
    const bool lstrbWasLow = !(prevPhases_ & kBitLSTRB);
    const bool lstrbNowHi  =  (phases_     & kBitLSTRB);
    if (lstrbWasLow && lstrbNowHi) {
        strobeWriteRegister(regSelect());
    }
}

void Sony35Drive::strobeWriteRegister(uint8_t reg)
{
    // Decode per the table at the top of this file. Effects on POM2's
    // internal state machine are the minimum needed for the //c+
    // SmartPort probe; finer-grained behaviour (step debouncing, eject
    // animation, RPM ramp-up) is deferred.
    switch (reg) {
        case 0x0: directionIn_ = true;  break;       // direction inward
        case 0x1:                                     // step
            if (directionIn_ && track_ > 0)  --track_;
            if (!directionIn_ && track_ < 79) ++track_;
            break;
        case 0x2: motorOn_ = true;  break;            // motor on
        case 0x3: motorOn_ = false; break;            // motor off
        case 0x4:                                     // eject
            if (image_ && image_->isLoaded()) {
                image_->eject();
                diskSwitched_ = true;
                pom2::log().info("Sony35", "eject requested by host");
            }
            break;
        case 0x8: side1_ = false; break;              // head 0
        case 0x9: side1_ = true;  break;              // head 1
        default:
            // Unmapped register — MAME logs but does nothing.
            break;
    }
}

bool Sony35Drive::senseR() const
{
    // Active-low logic on the SENSE line. Each register returns 1
    // (HIGH) for "condition NOT asserted". Lines marked "reserved"
    // return 1 (MAME also returns the default 1 for those).
    const uint8_t reg = regSelect();
    switch (reg) {
        case 0x0:                                       // /DIRTN
            return !directionIn_;
        case 0x1:                                       // /STEP — 1 = step done
            return true;
        case 0x2:                                       // /MOTOR ON — 0 when on
            return !motorOn_;
        case 0x3:                                       // /TRACK0 — 0 at trk 0
            return track_ != 0;
        case 0x4:                                       // /SWITCHED — 0 = just switched
            // Latching flip-flop: stays 0 until read once, then snaps
            // back to 1. The //c+ firmware uses this to drive its
            // "media changed" SmartPort status.
            if (diskSwitched_) {
                // MAME clears the latch on read; we mirror that. The
                // const-cast is a controlled exception — `senseR` is
                // logically a state-changing read on real hardware.
                const_cast<Sony35Drive*>(this)->diskSwitched_ = false;
                return false;
            }
            return true;
        case 0x6:                                       // /TACH
            // Reserved on stock 800K drive; 1 = no tach pulse.
            return true;
        case 0x8:                                       // /SIDES — 0 = double-sided
            return false;                               // 800K Sony is always 2-sided
        case 0xA:                                       // /READY — 0 = ready
            return !(image_ && image_->isLoaded() && motorOn_);
        case 0xB:                                       // /INSERTED — 0 = disk in
            return !(image_ && image_->isLoaded());
        case 0xD:                                       // /SEL
            return !sel_;
        case 0xF:                                       // /DRVIN — 0 = drive present
            return false;                               // present
        default:
            return true;                                // reserved → high
    }
}

}  // namespace pom2

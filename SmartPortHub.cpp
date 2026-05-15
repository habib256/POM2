// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Port of MAME `apple2e.cpp:638-679 apple2e_state::recalc_active_device`
// + the surrounding `phases_w / devsel_w / sel35_w` callback wiring
// (lines 625-637).

#include "SmartPortHub.h"
#include "IWMDevice.h"
#include "Sony35Drive.h"
#include "Logger.h"

namespace pom2 {

SmartPortHub::SmartPortHub() = default;

void SmartPortHub::attach(IWMDevice* iwm)
{
    iwm_ = iwm;
    if (!iwm_) return;
    iwm_->setPhasesCallback([this](uint8_t p) { onIwmPhases(p); });
    iwm_->setDevselCallback([this](uint8_t d) { onIwmDevsel(d); });
    // sel35Cb_ is unused by MAME (`apple2e.cpp:633-635` defines
    // sel35_w as an empty stub — the MIG's $C240/$C260 toggles do
    // the work). We keep the callback wired for symmetry but ignore.
    iwm_->setSel35Callback([](bool) {});
}

void SmartPortHub::setSony35(Sony35Drive* internal, Sony35Drive* external)
{
    drive35Internal_ = internal;
    drive35External_ = external;
    recalcActiveDevice();
}

void SmartPortHub::setMig35Sel(bool sel35)
{
    if (sel35_ == sel35) return;
    sel35_ = sel35;
    recalcActiveDevice();
}

void SmartPortHub::setMigIntDrive(bool intDrive)
{
    if (intDrive_ == intDrive) return;
    intDrive_ = intDrive;
    recalcActiveDevice();
}

void SmartPortHub::setMigHdSel(bool hdSel)
{
    hdSel_ = hdSel;
    // Head select is forwarded to the *currently* active 3.5" drive
    // (MAME line 676 `m_cur_floppy->ss_w(m_hdsel ? 1 : 0)` runs at the
    // end of recalc_active_device, so head changes always re-evaluate
    // on the current floppy).
    if (active35_) active35_->ssW(hdSel_);
}

void SmartPortHub::onIwmMotor(bool motorOn)
{
    // Forward motor state to BOTH 3.5" drives. Only the active drive
    // actually responds, but MAME also calls `mon_w` only on
    // m_cur_floppy. POM2 keeps both in sync so a quick devsel flip
    // doesn't leave a Sony spinning ghost.
    if (drive35Internal_) drive35Internal_->monW(!motorOn);
    if (drive35External_) drive35External_->monW(!motorOn);
}

void SmartPortHub::reset()
{
    devsel_   = 0;
    sel35_    = false;
    intDrive_ = false;
    hdSel_    = false;
    active35Selected_ = false;
    active35_         = nullptr;
    if (drive35Internal_) drive35Internal_->reset();
    if (drive35External_) drive35External_->reset();
}

void SmartPortHub::recalcActiveDevice()
{
    // MAME line 644-666 routing table.
    bool         is35  = false;
    Sony35Drive* drive = nullptr;

    if (devsel_ == 1) {
        if (!sel35_) {
            // 5.25" internal — DiskIICard handles. No 3.5" drive engaged.
        } else {
            // 3.5" external #2 — MAME `m_floppy[3]`. POM2 doesn't
            // model a second external 3.5" yet; leave drive=nullptr.
        }
    } else if (devsel_ == 2) {
        if (intDrive_) {
            is35  = true;
            drive = drive35Internal_;        // //c+ on-board 3.5"
        } else if (!sel35_) {
            // 5.25" external — DiskIICard handles.
        } else {
            // 3.5" external #1 — also `m_floppy[1]`? MAME line 665
            // labels it "should be external 3.5 #2, for a 3rd drive"
            // and sets nullptr. Mirror that — POM2 has no 3rd drive.
        }
    }

    // When the active floppy changes, MAME flushes the IWM and pushes
    // the new pointer to `m_iwm->set_floppy`. POM2's IWM set_floppy
    // takes a DiskImage* today and is only used for the 5.25" data
    // path, so we deliberately don't repoint it for 3.5" — Phase 2
    // will extend IWMDevice with a polymorphic floppy target. For now
    // we just route phase strobes + head select.
    active35Selected_ = is35;
    active35_         = is35 ? drive : nullptr;

    if (active35_) {
        active35_->setSel(iwm_ && iwm_->sel());
        active35_->ssW(hdSel_);
    }
}

void SmartPortHub::onIwmPhases(uint8_t phases)
{
    // MAME `apple2e.cpp:625-631 phases_w`: forward to the current
    // floppy. POM2's 5.25" path is owned by DiskIICard (which has its
    // own seekPhaseW hook from the slot-6 mux); 3.5" goes through
    // Sony35Drive.
    if (active35_) {
        active35_->setSel(iwm_ && iwm_->sel());
        active35_->seekPhaseW(phases);
    }
}

void SmartPortHub::onIwmDevsel(uint8_t devsel)
{
    if (devsel_ == devsel) return;
    devsel_ = devsel;
    recalcActiveDevice();
}

}  // namespace pom2

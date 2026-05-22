// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MemoryProfile — strategy for machine-profile-specific memory behaviour
// that would otherwise leak into Memory's generic dispatcher.
//
// Today only the //c-class (16 KB rev-255 //c, 32 KB //c rev-0/3/4, and
// //c+) needs one: alt-firmware bank switch ($C028 ROMBANK), on-board
// IWM ($C0E0-$C0EF), //c+ MIG windows ($CC00/$CE00), forced INTCXROM
// (no physical slots), and the alt-firmware override of $C100-$FFFF.
// II/II+/IIe run with `Memory::iicProfile_ == nullptr` — a single
// `if (iicProfile_)` branch on the hot path, **zero virtual calls** on
// those profiles. See MemoryProfile_IIcClass for the implementation and
// DEV.md § Memory.
#pragma once

#include <cstdint>

namespace pom2 { class IWMDevice; class SmartPortHub; }

class MemoryProfile {
public:
    virtual ~MemoryProfile() = default;

    // $C100-$CFFF gate: //c-class forces INTCXROM (it has no slots).
    virtual bool forcesIntCxRom() const = 0;

    // Ctrl-Reset / power-on: drop ROMBANK back to bank 0 (cold-start).
    virtual void onResetSoftSwitches() = 0;

    // $C02x ROMBANK toggle. Returns true (consumed) on //c-class so the
    // caller skips the II/II+ cassette-output fallback.
    virtual bool romBankToggle() = 0;

    // $C0E0-$C0EF on-board IWM. Read: advances the IWM state machine and,
    // when authoritative, sets `out` and returns true; returns false in
    // shadow mode or with no media/IWM (caller falls back to the slot-6
    // LSS byte). Write: dispatches the byte to the IWM.
    virtual bool ioReadIWM (uint16_t addr, uint64_t cyc, uint8_t& out) = 0;
    virtual void ioWriteIWM(uint16_t addr, uint8_t value, uint64_t cyc) = 0;

    // $C100-$CFFF under INTCXROM. Returns true + `out` for //c+ MIG
    // windows ($CC00/$CE00) or alt-firmware bank 1; false ⇒ caller
    // returns its internal I/O ROM byte. `floatBus` is the floating-bus
    // value used for MIG read fall-through (MIG control reads float).
    virtual bool internalRomRead(uint16_t addr, uint8_t floatBus, uint8_t& out) = 0;
    // Returns true if the write hit a MIG window (side effects applied);
    // false ⇒ caller absorbs the write (internal ROM is read-only).
    virtual bool internalRomWrite(uint16_t addr, uint8_t value) = 0;

    // $D000-$FFFF ROM reads (lcReadRam == false): alt-firmware bank 1
    // overrides motherboard ROM. Returns true + `out`, else false.
    virtual bool languageCardRomRead(uint16_t addr, uint8_t& out) = 0;

    // Façade forwards from Memory::setIWM / setSmartPortHub /
    // setIWMAuthoritative (kept for test/back-compat — see DEV.md).
    virtual void setIwm(pom2::IWMDevice* iwm) = 0;
    virtual void setSmartPortHub(pom2::SmartPortHub* hub) = 0;
    virtual void setIwmAuthoritative(bool on) = 0;
};

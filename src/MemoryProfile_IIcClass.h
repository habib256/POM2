// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// IIcClassProfile — MemoryProfile for the //c family: 16 KB rev-255 //c,
// 32 KB //c rev-0/3/4, and //c+. Holds the //c-specific state that used
// to live in Memory (alt-firmware bank, ROMBANK flag, //c+ MIG gate
// array) and the on-board IWM / SmartPort hub pointers. The MIG decode
// (`migRead`/`migWrite`) is a verbatim port of MAME
// `apple2e.cpp:532-624` (apple2e_state::mig_r / mig_w).
#pragma once

#include "MemoryProfile.h"

#include <array>
#include <cstddef>

namespace pom2 { class IWMDevice; class SmartPortHub; }

class IIcClassProfile final : public MemoryProfile {
public:
    // `payload` = 16 KB firmware covering $C000-$FFFF (bank 0, active at
    // reset). `payloadSize` >= 0x3c00 (guaranteed by Memory's //c-class
    // probe). `altBank16k` = upper 16 KB (bank 1) for the $C028 toggle,
    // or nullptr (16 KB rev-255 //c has no alt bank). //c+ is probed from
    // `payload[0x3bbf] == 0x05` (MAME `apple2e.cpp:1275-1299`).
    IIcClassProfile(const uint8_t* payload, std::size_t payloadSize,
                    const uint8_t* altBank16k,
                    pom2::IWMDevice* iwm, pom2::SmartPortHub* hub,
                    bool iwmAuthoritative);

    bool forcesIntCxRom() const override { return true; }
    void onResetSoftSwitches() override  { romBank_ = false; }
    bool romBankToggle() override;

    bool ioReadIWM (uint16_t addr, uint64_t cyc, uint8_t& out) override;
    void ioWriteIWM(uint16_t addr, uint8_t value, uint64_t cyc) override;

    bool internalRomRead (uint16_t addr, uint8_t floatBus, uint8_t& out) override;
    bool internalRomWrite(uint16_t addr, uint8_t value) override;
    bool languageCardRomRead(uint16_t addr, uint8_t& out) override;

    void setIwm(pom2::IWMDevice* iwm) override            { iwm_ = iwm; }
    void setSmartPortHub(pom2::SmartPortHub* hub) override { hub_ = hub; }
    void setIwmAuthoritative(bool on) override            { iwmAuthoritative_ = on; }

private:
    uint8_t migRead (uint16_t migOffset, uint8_t floatBus);
    void    migWrite(uint16_t migOffset, uint8_t value);

    // Alt firmware (bank 1) for the $C028 ROMBANK toggle. Bytes
    // 0x100-0x0FFF mirror $C100-$CFFF (alt internal I/O ROM); bytes
    // 0x1000-0x3FFF mirror $D000-$FFFF (alt firmware).
    std::array<uint8_t, 0x4000> altFirmware_{};
    bool     hasAltBank_ = false;   // only 32 KB dumps provide bank 1
    bool     romBank_    = false;   // false = bank 0 (cold-start), true = bank 1
    bool     isPlus_     = false;   // //c+ — gates the MIG windows

    // Apple MIG gate array (//c+). See MAME `apple2e.cpp:532-624`.
    std::array<uint8_t, 0x800> migRam_{};
    uint16_t migPage_     = 0;
    bool     migIntDrive_ = false;
    bool     migHdSel_    = false;

    pom2::IWMDevice*    iwm_ = nullptr;
    pom2::SmartPortHub* hub_ = nullptr;
    bool               iwmAuthoritative_ = true;
};

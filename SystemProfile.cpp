// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SystemProfile.h"

namespace pom2 {

namespace {

// ROMs are user-provided (Apple's copyright). POM2 ships with no ROM —
// the user drops their dumps into roms/. Each profile's probe list
// supports a few historical filenames so a user who downloaded a
// "system_rom_pack" from anywhere reasonable still gets a match. The
// candidate order is: machine-specific first (apple2o = original,
// apple2p = plus, apple2e, apple2c-32K/16K), then the generic
// `apple2.rom` fallback for legacy POM2 installs.

const ProfileConfig& cfgAppleII()
{
    static const ProfileConfig cfg{
        SystemProfile::AppleII,
        "ii",
        "Apple ][ Original (1977)",
        { "roms/apple2o.rom", "roms/apple2.rom" },
        { "roms/apple2_char.rom" },
        /*iieMode=*/false,
        M6502::CpuMode::NMOS,
        17045,
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIPlus()
{
    static const ProfileConfig cfg{
        SystemProfile::AppleIIPlus,
        "ii+",
        "Apple ][+ (1979)",
        { "roms/apple2p.rom", "roms/apple2.rom" },
        { "roms/apple2_char.rom" },
        /*iieMode=*/false,
        M6502::CpuMode::NMOS,
        17045,
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIe()
{
    static const ProfileConfig cfg{
        SystemProfile::AppleIIe,
        "iie",
        "Apple //e Enhanced (1985)",
        { "roms/apple2e.rom" },
        // Apple //e Enhanced char ROM (4 KB) carries mousetext + lowercase;
        // fall back to the 2 KB II/II+ ROM if the user only has the older
        // dump (POM2's `loadCharRom` normalises both to AppleWin csbits).
        { "roms/apple2e_char.rom", "roms/apple2_char.rom" },
        /*iieMode=*/true,
        // Enhanced //e ships a 65C02. The non-enhanced //e (1983) used
        // a 6502 NMOS — most software targets Enhanced, so we default
        // to CMOS here. Manual override is available via Debug → CPU.
        M6502::CpuMode::CMOS,
        17045,
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIc()
{
    static const ProfileConfig cfg{
        SystemProfile::AppleIIc,
        "iic",
        "Apple //c (1984)",
        // //c shipped with multiple ROM revisions (255, 0, 3, 4, X). The
        // 16 KB dump = ROM 255 (original), 32 KB Kv0 = ROM 0/3/4 (later
        // revisions with bigger mousetext + AppleTalk hooks). Try the
        // larger one first to get the full feature set.
        { "roms/apple2c-32Kv0.rom", "roms/apple2c-16K.rom" },
        { "roms/apple2e_char.rom", "roms/apple2_char.rom" },
        /*iieMode=*/true,        // same paging as IIe
        M6502::CpuMode::CMOS,    // //c always shipped 65C02
        17045,
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIcPlus()
{
    static const ProfileConfig cfg{
        SystemProfile::AppleIIcPlus,
        "iic+",
        "Apple //c Plus (1988)",
        // //c Plus shipped with a 32 KB ROM (ROM X4). The "ROM 5" dump
        // some users have isn't the IIc Plus, that's a IIc revision —
        // double-check via the displayed boot screen. Falls back to
        // the IIc 32K dump if no IIc Plus-specific ROM is present.
        { "roms/apple2cp.rom", "roms/apple2c-plus.rom",
          "roms/apple2c-32Kv0.rom" },
        { "roms/apple2e_char.rom", "roms/apple2_char.rom" },
        /*iieMode=*/true,        // same paging as IIe/IIc
        M6502::CpuMode::CMOS,    // 65C02 at 4 MHz on real silicon
        // Real //c Plus boots with its on-board Zip-style accelerator
        // running the 65C02 at ~4 MHz (the slower 1 MHz mode is only
        // entered for disk I/O via $C036 on real silicon — POM2 doesn't
        // model that softswitch, but its event-driven disk LSS is purely
        // cycle-driven so a 4× CPU still produces correctly-paced
        // nibbles). 4 × 17045 = 68180 cycles per 60 Hz frame.
        68180,
    };
    return cfg;
}

}  // namespace

const ProfileConfig& profileConfig(SystemProfile p)
{
    switch (p) {
        case SystemProfile::AppleII:      return cfgAppleII();
        case SystemProfile::AppleIIPlus:  return cfgAppleIIPlus();
        case SystemProfile::AppleIIe:     return cfgAppleIIe();
        case SystemProfile::AppleIIc:     return cfgAppleIIc();
        case SystemProfile::AppleIIcPlus: return cfgAppleIIcPlus();
    }
    return cfgAppleIIPlus();   // unreachable, silences compiler
}

SystemProfile profileFromKey(std::string_view key)
{
    if (key == "ii"   || key == "apple2"   || key == "appleii")     return SystemProfile::AppleII;
    if (key == "ii+"  || key == "iiplus"   || key == "apple2plus"
        || key == "appleiiplus" || key == "ii-plus")                return SystemProfile::AppleIIPlus;
    if (key == "iie"  || key == "apple2e"  || key == "appleiie"
        || key == "//e")                                            return SystemProfile::AppleIIe;
    if (key == "iic"  || key == "apple2c"  || key == "appleiic"
        || key == "//c")                                            return SystemProfile::AppleIIc;
    if (key == "iic+" || key == "iicplus" || key == "apple2cplus"
        || key == "apple2cp" || key == "//c+"
        || key == "appleiicplus")                                   return SystemProfile::AppleIIcPlus;
    return SystemProfile::AppleIIPlus;
}

std::string_view profileKey(SystemProfile p)
{
    return profileConfig(p).key;
}

const std::array<SystemProfile, 5>& allProfiles()
{
    static const std::array<SystemProfile, 5> all = {
        SystemProfile::AppleII,
        SystemProfile::AppleIIPlus,
        SystemProfile::AppleIIe,
        SystemProfile::AppleIIc,
        SystemProfile::AppleIIcPlus,
    };
    return all;
}

}  // namespace pom2

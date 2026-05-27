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
        {},   // builtInSlots: all 7 slots free (real Apple ][ exposed 8 physical slots)
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
        {},   // all slots free
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIeUnenhanced()
{
    static const ProfileConfig cfg{
        SystemProfile::AppleIIeUnenhanced,
        "iie-u",
        "Apple //e (1983, Unenhanced)",
        // Apple part 342-0135-B (D000/D8) + 342-0134-A (E000/E8/F0/F8) =
        // 16 KB original //e firmware. MAME loads them via
        // `apple2e.cpp:5520-5544 ROM_START(apple2e)`. Fall back to the
        // generic apple2e.rom only if the dedicated dump is absent — but
        // beware: that fallback is most likely the Enhanced firmware, so
        // a CRC mismatch will be logged (Theme 9, ROM identity check).
        { "roms/apple2e_unenh.rom", "roms/342-0135-b.64.rom",
          "roms/apple2e.rom" },
        // 1983 //e shipped the 2 KB char ROM (no mousetext). 4 KB
        // Enhanced char ROM is rejected for fidelity — the mousetext
        // glyphs at $40-$5F when ALTCHAR=on would be wrong on Unenhanced.
        { "roms/apple2e_char_2k.rom", "roms/341-0265-a.chr.rom",
          "roms/apple2_char.rom" },
        /*iieMode=*/true,
        // Unenhanced //e = NMOS 6502 (no STZ/BRA/PHX/PLX, no decimal-mode
        // fix). Software that uses CMOS opcodes will trap to NOP/KIL.
        M6502::CpuMode::NMOS,
        17045,
        {},   // all 7 slots free (//e has real expansion slots)
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
        // Enhanced //e ships a 65C02. The non-enhanced //e (1983) uses
        // the AppleIIeUnenhanced profile (NMOS 6502, 2 KB char ROM).
        M6502::CpuMode::CMOS,
        17045,
        {},   // all 7 slots free
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIc()
{
    // Real //c has NO physical expansion slots — MAME's apple2c()
    // (`apple2e.cpp:5168-5188`) explicitly `device_remove("sl1")` through
    // sl7, then re-adds onboard MOCKINGBOARD at sl4 and DISKIING at sl6.
    // The slot IDs are virtual (firmware $C100/$C200 = SSC printer/modem
    // ports, $C400 = Mouse, $C600 = Disk II), and the user must NOT be
    // able to unplug them via the Slot Configuration UI — doing so would
    // wedge the //c boot path.
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
        // builtInSlots: [_, printer, ssc, _, mouse, smartport35, diskii, _]
        // sl1 = built-in printer (POM2-synthetic parallel-style). On real
        // //c hardware $C100 was a *second* SSC (firmware labelled it the
        // "printer port" but it was serial); we substitute a synthetic
        // parallel printer that spools to a host file (.txt / .pdf) so
        // PR#1 from BASIC has a useful sink out of the box — the same
        // "print to PDF" affordance macOS bakes into its print dialog.
        // sl5 = built-in SmartPort (the 32 KB ROM 0/3/4 //c shipped with
        // SmartPort firmware here for an external 3.5"/hard disk). POM2
        // serves it as a host-backed block device so 3.5" + HDV boot via
        // SmartPort — the real IWM/Sony GCR boot path is unmodelled (see
        // project_iic_smartport_boot). sl7 left free for power users;
        // sl3 is the internal 80-col firmware area covered by the AUX label.
        {
            std::nullopt,                                // sl0 reserved
            BuiltInSlot{"printer", "built-in printer"},  // sl1
            BuiltInSlot{"ssc",    "built-in serial"},    // sl2
            std::nullopt,                                // sl3 (AUX 80-col label)
            // sl4: AppleWin-style HLE mouse — the real //c on-board mouse
            // shares the same firmware-visible API as the slot card but the
            // MAME-fidelity M68705 emulation isn't a meaningful target here
            // (no replaceable MCU on the //c), so the lighter HLE variant
            // is the right built-in default.
            BuiltInSlot{"mouseaw",  "built-in mouse"},   // sl4
            BuiltInSlot{"smartport35", "built-in SmartPort"}, // sl5
            BuiltInSlot{"diskii", "built-in Disk II"},   // sl6
            std::nullopt,                                // sl7
        },
    };
    return cfg;
}

const ProfileConfig& cfgAppleIIcPlus()
{
    // Like the //c, the //c+ has no physical expansion slots. MAME's
    // apple2cp() (`apple2e.cpp:5229-5249`) starts from apple2c() and
    // additionally removes sl4 + sl6 to instantiate the IWM directly.
    // The //c+ adds an on-board SmartPort 3.5" path at slot 5 (firmware
    // bank 1 at $C500) on top of the //c's serial + mouse + Disk II.
    static const ProfileConfig cfg{
        SystemProfile::AppleIIcPlus,
        "iic+",
        "Apple //c Plus (1988)",
        // //c+ shipped with a 32 KB ROM X4 (`apple2cp.rom`). The boot
        // path needs the MIG (Multidrive Interface Glue, MAME
        // `apple2e.cpp:532-624 mig_r/mig_w`) and a proper IWM at
        // $C0E0-$C0EF — see Memory's $CC00/$CE00 MIG window in
        // romswitch=on mode and the IWM hooks on DiskIICard. Falls
        // back to the older //c 32 KB dump if no //c+-specific ROM
        // exists (the //c probe order's last entry).
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
        // builtInSlots: [_, printer, ssc, _, mouse, smartport35, diskii, _]
        // sl1 = built-in printer (same POM2-synthetic substitution as on
        // the //c — see cfgAppleIIc comment).
        {
            std::nullopt,
            BuiltInSlot{"printer",     "built-in printer"},        // sl1
            BuiltInSlot{"ssc",         "built-in serial"},         // sl2
            std::nullopt,                                          // sl3 (AUX)
            BuiltInSlot{"mouseaw",     "built-in mouse"},          // sl4
            BuiltInSlot{"smartport35", "built-in SmartPort 3.5\""}, // sl5
            BuiltInSlot{"diskii",      "built-in Disk II (IWM)"},  // sl6
            std::nullopt,                                          // sl7
        },
    };
    return cfg;
}

}  // namespace

const ProfileConfig& profileConfig(SystemProfile p)
{
    switch (p) {
        case SystemProfile::AppleII:            return cfgAppleII();
        case SystemProfile::AppleIIPlus:        return cfgAppleIIPlus();
        case SystemProfile::AppleIIeUnenhanced: return cfgAppleIIeUnenhanced();
        case SystemProfile::AppleIIe:           return cfgAppleIIe();
        case SystemProfile::AppleIIc:           return cfgAppleIIc();
        case SystemProfile::AppleIIcPlus:       return cfgAppleIIcPlus();
    }
    return cfgAppleIIPlus();   // unreachable, silences compiler
}

SystemProfile profileFromKey(std::string_view key)
{
    if (key == "ii"   || key == "apple2"   || key == "appleii")     return SystemProfile::AppleII;
    if (key == "ii+"  || key == "iiplus"   || key == "apple2plus"
        || key == "appleiiplus" || key == "ii-plus")                return SystemProfile::AppleIIPlus;
    if (key == "iie-u" || key == "iie-unenh" || key == "iie-unenhanced"
        || key == "iieunenhanced" || key == "apple2e-1983"
        || key == "//e-u")                                          return SystemProfile::AppleIIeUnenhanced;
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

const std::array<SystemProfile, 6>& allProfiles()
{
    static const std::array<SystemProfile, 6> all = {
        SystemProfile::AppleII,
        SystemProfile::AppleIIPlus,
        SystemProfile::AppleIIeUnenhanced,
        SystemProfile::AppleIIe,
        SystemProfile::AppleIIc,
        SystemProfile::AppleIIcPlus,
    };
    return all;
}

}  // namespace pom2

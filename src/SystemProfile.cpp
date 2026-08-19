// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SystemProfile.h"

namespace pom2 {

namespace {

// The repo and release packages ship a full roms/ tree (see
// packaging/bundle.manifest); a user can still override any dump in the
// per-user data dir. Each profile's probe list
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
        //
        // `3420033a.256` is MAME's `apple2c0` part — the "//c (UniDisk 3.5)"
        // revision, i.e. the ROM whose firmware knows the external UniDisk
        // 3.5. It sits LAST on purpose: it is a fallback for users who own
        // only that dump, not an upgrade. POM2 still serves 3.5"/HDV on //c
        // through the host-side SmartPort at built-in slot 5 (the real IWM
        // bit-shift path is deliberately unmodelled — see CLAUDE.md), so
        // this ROM does not by itself unlock a hardware-accurate 3.5 boot.
        { "roms/apple2c-32Kv0.rom", "roms/apple2c-16K.rom",
          "roms/3420033a.256" },
        { "roms/apple2e_char.rom", "roms/apple2_char.rom" },
        /*iieMode=*/true,        // same paging as IIe
        M6502::CpuMode::CMOS,    // //c always shipped 65C02
        17045,
        // builtInSlots: [_, ssc, ssc, _, mouse, smartport35, diskii, _]
        // sl1 + sl2 = the //c's two on-board serial ports, both plugged
        // as `ssc` ("printer port" / "modem port" — see the detailed
        // comment on the sl1 entry below).
        // sl5 = built-in SmartPort (the 32 KB ROM 0/3/4 //c shipped with
        // SmartPort firmware here for an external 3.5"/hard disk). POM2
        // serves it as a host-backed block device so 3.5" + HDV boot via
        // SmartPort — the real IWM/Sony GCR boot path is unmodelled (see
        // project_iic_smartport_boot). sl7 left free for power users;
        // sl3 is the internal 80-col firmware area covered by the AUX label.
        {
            std::nullopt,                                // sl0 reserved
            // Real //c has TWO serial ports (both RS-232 via the on-board
            // Zilog SCC), exposed as SSC-compatible firmware at $C100
            // (printer port) and $C200 (modem port). Apple //c Technical
            // Reference Manual, app. A; MAME apple2e.cpp machine config
            // apple2c. POM2 used to put a parallel PrinterCard at sl1 —
            // wrong: the //c had no parallel port, the "printer" name
            // refers to the serial port wired to the ImageWriter DIN.
            BuiltInSlot{"ssc",    "built-in printer port (serial)"}, // sl1
            BuiltInSlot{"ssc",    "built-in modem port (serial)"},   // sl2
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
        /*noPhysicalSlots=*/true,
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
        // builtInSlots: [_, ssc(printer), ssc(modem), _, mouse,
        //                smartport35, diskii, _]. Same dual-serial layout
        // as cfgAppleIIc — //c+ inherits the //c's two on-board RS-232
        // ports (Apple //c+ Reference Addendum, MAME apple2cp config).
        {
            std::nullopt,
            BuiltInSlot{"ssc",         "built-in printer port (serial)"}, // sl1
            BuiltInSlot{"ssc",         "built-in modem port (serial)"},   // sl2
            std::nullopt,                                          // sl3 (AUX)
            BuiltInSlot{"mouseaw",     "built-in mouse"},          // sl4
            BuiltInSlot{"smartport35", "built-in SmartPort 3.5\""}, // sl5
            BuiltInSlot{"diskii",      "built-in Disk II (IWM)"},  // sl6
            std::nullopt,                                          // sl7
        },
        /*noPhysicalSlots=*/true,
    };
    return cfg;
}

// European PAL variants. Same ROMs / CPU / slot layout as their NTSC siblings
// — only the machine timing differs (312 lines, ~50 Hz, 20313 cyc/frame). We
// derive them by copying the NTSC config and overriding the timing fields, so
// the (long) //c slot table is never duplicated.
const ProfileConfig& cfgAppleIIePAL()
{
    static const ProfileConfig cfg = [] {
        ProfileConfig c = cfgAppleIIe();
        c.profile              = SystemProfile::AppleIIePAL;
        c.key                  = "iie-pal";
        c.displayName          = "Apple //e Enhanced PAL (50 Hz)";
        c.defaultCyclesPerFrame = 20313;
        c.videoStandard        = VideoStandard::PAL;
        return c;
    }();
    return cfg;
}

const ProfileConfig& cfgAppleIIcPAL()
{
    static const ProfileConfig cfg = [] {
        ProfileConfig c = cfgAppleIIc();
        c.profile              = SystemProfile::AppleIIcPAL;
        c.key                  = "iic-pal";
        c.displayName          = "Apple //c PAL (Le Chat Mauve)";
        c.defaultCyclesPerFrame = 20313;
        c.videoStandard        = VideoStandard::PAL;
        // This profile IS the European //c fitted with the Le Chat Mauve RGB
        // "Adaptateur IIc" on its DB-15 video-expansion connector — the card
        // that gives it Péritel RGB output (the whole reason the profile
        // exists). Wire it as an on-board fixture at sl7 (the canonical Chat
        // Mauve slot) so the adapter is always present, like the //c's other
        // built-ins. Plain //c (NTSC) leaves it user-pluggable instead.
        c.builtInSlots[7] = BuiltInSlot{
            "chatmauve", "built-in Le Chat Mauve RGB (Adaptateur IIc)"};
        return c;
    }();
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
        case SystemProfile::AppleIIePAL:        return cfgAppleIIePAL();
        case SystemProfile::AppleIIcPAL:        return cfgAppleIIcPAL();
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
    if (key == "iie-pal" || key == "iiepal" || key == "apple2e-pal"
        || key == "//e-pal")                                        return SystemProfile::AppleIIePAL;
    if (key == "iic-pal" || key == "iicpal" || key == "apple2c-pal"
        || key == "//c-pal" || key == "chatmauve")                  return SystemProfile::AppleIIcPAL;
    return SystemProfile::AppleIIPlus;
}

std::string_view profileKey(SystemProfile p)
{
    return profileConfig(p).key;
}

bool slotKeyIsUserChoice(const ProfileConfig& cfg, int slot,
                         std::string_view cardKey, std::string_view savedKey)
{
    if (slot < 1 || slot > 7) return false;
    // Profile-forced built-in (//c/+ on-board SSC / Mouse / SmartPort /
    // Disk II, //c PAL's on-board Le Chat Mauve): the live slot map holds
    // the PROFILE's card, not the user's saved choice.
    if (cfg.builtInSlots[static_cast<size_t>(slot)].has_value()) return false;
    // No-physical-slots machine (//c, //c+): the non-built-in connectors
    // don't exist, so the live map is force-emptied at profile-apply time.
    // The ONE user-controllable peripheral there is the Le Chat Mauve rear
    // DB-15 adapter — that choice persists in BOTH directions: writing
    // "chatmauve", and writing "" over a previously-saved "chatmauve"
    // (= the user removed the adapter; without the savedKey clause the
    // removal could never stick and the stale key resurrected the adapter
    // on every launch). An "" over anything else stays skipped so the
    // force-emptied virtual slots never clobber a //e card layout.
    if (cfg.noPhysicalSlots && cardKey != "chatmauve" &&
        savedKey != "chatmauve") return false;
    return true;
}

const std::array<SystemProfile, 8>& allProfiles()
{
    static const std::array<SystemProfile, 8> all = {
        SystemProfile::AppleII,
        SystemProfile::AppleIIPlus,
        SystemProfile::AppleIIeUnenhanced,
        SystemProfile::AppleIIe,
        SystemProfile::AppleIIc,
        SystemProfile::AppleIIcPlus,
        SystemProfile::AppleIIePAL,
        SystemProfile::AppleIIcPAL,
    };
    return all;
}

}  // namespace pom2

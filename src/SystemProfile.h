// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SystemProfile — canonical Apple II model selection. Each profile
// resolves to:
//   * a CPU type (NMOS 6502 or CMOS 65C02)
//   * a ROM probe order (first existing file wins)
//   * a charset ROM probe order
//   * a Memory IIe paging flag (off for II/II+, on for IIe/IIc)
//   * a default cycles-per-frame (1.023 MHz for II/II+/IIe/IIc; the
//     IIc Plus defaults to 4× = ~4 MHz to match real silicon)
//
// Profile switching happens via `MainWindow::applyProfile()` which does
// a full cold-reset: stops the CPU worker, wipes RAM + soft switches,
// re-loads the new ROM, re-plugs the default slot cards, and restarts
// from the new reset vector. Disk and HDV image MOUNT PATHS are
// preserved across the switch so the user can test the same software
// stack under different machines without re-mounting.

#ifndef POM2_SYSTEM_PROFILE_H
#define POM2_SYSTEM_PROFILE_H

#include "M6502.h"
#include "CpuClock.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

/// A slot occupied by construction on this machine (e.g. on-board Disk II
/// at slot 6 on a //c, or the IWM SmartPort at slot 5 on a //c+). When a
/// profile's `builtInSlots[N]` carries a `BuiltInSlot`, the slot card is
/// force-plugged with `cardKey` regardless of user settings, and the
/// Slot Configuration UI renders the row read-only with `label` as a
/// badge so the user knows what's there but can't edit it.
struct BuiltInSlot {
    std::string cardKey;   // matches a kCardTypes[] key in MainWindow_Slots
    std::string label;     // user-visible suffix in the panel
};

enum class SystemProfile {
    AppleII,             // Apple II original (1977), Integer BASIC, 6502 NMOS
    AppleIIPlus,         // Apple II+ (1979), Applesoft + Autostart, 6502 NMOS
    AppleIIeUnenhanced,  // Apple //e (1983), 6502 NMOS, no-mousetext char ROM
    AppleIIe,            // Apple //e Enhanced (1985), 65C02, mousetext char ROM
    AppleIIc,            // Apple //c (1984), 65C02, IIe-class soft switches
    AppleIIcPlus,        // Apple //c Plus (1988), 65C02 @ 4 MHz, built-in SmartPort
    // European PAL variants (50 Hz, 312 lines): same ROMs/CPU as their NTSC
    // siblings, but PAL machine timing. The //c PAL is the machine that took
    // the Le Chat Mauve RGB Péritel adapter on its DB-15 port; //e PAL is the
    // French Touch / DIX target. See CpuClock.h VideoStandard.
    AppleIIePAL,         // Apple //e Enhanced PAL (65C02, 312 lines, 50 Hz)
    AppleIIcPAL,         // Apple //c PAL (Le Chat Mauve RGB adapter machine)
};

struct ProfileConfig {
    SystemProfile          profile;
    std::string_view       key;             // canonical persistence key
    std::string_view       displayName;
    std::vector<std::string> romProbeOrder;   // 16/32 KB main ROM
    std::vector<std::string> charRomProbeOrder;
    bool                   iieMode;          // Memory::setIIEMode(...)
    M6502::CpuMode         defaultCpu;
    int                    defaultCyclesPerFrame;
    // Indexed by slot $C1-$C7 (entries 1..7 used; entry 0 reserved for the
    // language card sl0 if ever modelled as a SlotPeripheral). A populated
    // entry means "the on-board hardware lives here and the user cannot
    // swap it". An empty entry (nullopt) means the slot is free.
    std::array<std::optional<BuiltInSlot>, 8> builtInSlots;
    // True for machines with NO physical expansion bus — //c and //c+.
    // On these, even the "empty" `builtInSlots` entries (sl3 / sl7) are
    // not user-pluggable: the slot connector simply doesn't exist on
    // real hardware, so POM2 force-empties any non-builtIn slot at
    // profile-apply time and locks the Slot Config picker greyed out.
    bool                   noPhysicalSlots = false;
    // Machine video standard. NTSC (262 lines, 60 Hz) for US machines; PAL
    // (312 lines, ~50 Hz) for the European //e PAL / //c PAL. Appended last
    // and defaulted so existing positional initializers stay valid.
    VideoStandard          videoStandard = VideoStandard::NTSC;
};

/// Resolve a profile enum to its full configuration. The probe orders
/// are ordered by preference; the caller's job is to pick the first
/// existing file at runtime. All four profiles are always defined —
/// missing ROM files don't disable the profile, they just degrade the
/// machine to "no ROM" status (the user sees `NO ROM` in the title
/// bar and the CPU starts running garbage at the reset vector).
const ProfileConfig& profileConfig(SystemProfile p);

/// Inverse — parse a persistence/CLI key to a profile enum. Returns
/// `AppleIIPlus` (the historical POM2 default) when the key is empty
/// or unrecognised. Accepts the canonical keys (`ii`, `ii+`, `iie`,
/// `iic`) and a few common aliases (`apple2`, `apple2plus`, `//e`, `//c`).
SystemProfile profileFromKey(std::string_view key);

/// Forward — get the persistence key for a profile.
std::string_view profileKey(SystemProfile p);

/// Should `slot_N_card` be persisted for `slot` holding `cardKey` under
/// this profile? False for profile-forced slots: built-in cards, and —
/// except for the user-pluggable "chatmauve" rear-connector adapter — the
/// force-emptied virtual connectors of a `noPhysicalSlots` machine. On
/// those slots the live mapping carries the PROFILE's value, and writing
/// it back would clobber the user's real saved choice (e.g. quitting on
/// //c used to overwrite slot_4_card=mockingboard with the on-board
/// "mouseaw"). Shared by the ~MainWindow persist loop and the Slot Config
/// panel's Apply button so both sites apply the identical guard.
bool slotKeyIsUserChoice(const ProfileConfig& cfg, int slot,
                         std::string_view cardKey);

/// All profiles in display order. Used by the Presets menu loop.
const std::array<SystemProfile, 8>& allProfiles();

}  // namespace pom2

#endif // POM2_SYSTEM_PROFILE_H

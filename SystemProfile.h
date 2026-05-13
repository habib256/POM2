// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SystemProfile — canonical Apple II model selection. Each profile
// resolves to:
//   * a CPU type (NMOS 6502 or CMOS 65C02)
//   * a ROM probe order (first existing file wins)
//   * a charset ROM probe order
//   * a Memory IIe paging flag (off for II/II+, on for IIe/IIc)
//   * a default cycles-per-frame (always 1.023 MHz; IIc Plus would be
//     4 MHz but POM2 doesn't model that variant)
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

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pom2 {

enum class SystemProfile {
    AppleII,       // Apple II original (1977), Integer BASIC, 6502 NMOS
    AppleIIPlus,   // Apple II+ (1979), Applesoft + Autostart, 6502 NMOS
    AppleIIe,      // Apple //e (1983, Enhanced revision targets 65C02)
    AppleIIc,      // Apple //c (1984), 65C02, IIe-class soft switches
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

/// All four profiles in display order. Used by the Presets menu loop.
const std::array<SystemProfile, 4>& allProfiles();

}  // namespace pom2

#endif // POM2_SYSTEM_PROFILE_H

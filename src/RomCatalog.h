// VERHILLE Arnaud 2026
//
// POM2 Apple II Emulator
// Copyright (C) 2026
//
// RomCatalog — what every ROM POM2 looks for actually IS.
//
// ROMs are user-provided (POM2 ships none), so "it doesn't boot" and "that
// card won't plug" are usually a missing or mis-named dump. The probe order
// for the MACHINE firmware + character generator already lives in
// `SystemProfile.h` (`romProbeOrder` / `charRomProbeOrder`) and the ROM
// Status panel reads it straight from there — one source of truth, so a new
// profile shows up without touching this file.
//
// What is NOT expressible there is the peripheral side: each card probes its
// own candidates at its own plug site in MainWindow.cpp / ClockCard.cpp. This
// table names those, in the same order the code tries them, with what the
// dump is and what happens when it is absent — that last column is the one
// that matters, because most card ROMs are optional and degrade rather than
// fail.
//
// `size` is the only hard check: a Disk II PROM is 256 bytes and a Grappler+
// EPROM is 4 KB, full stop, so a mismatch is a wrong file, not a variant.
// CRC32 is shown for identification but only *asserted* where POM2 has a
// documented reference dump to compare against (see `knownCrc`).

#ifndef POM2_ROM_CATALOG_H
#define POM2_ROM_CATALOG_H

#include <cstdint>
#include <vector>

namespace pom2 {

/// One catalogued ROM: what it is, where POM2 looks, how big it must be.
struct RomCatalogEntry {
    const char* group;        ///< Section heading in the panel.
    const char* name;         ///< Human name of the part.
    /// Candidate paths, in the order the code probes them. The first that
    /// resolves through ResourcePaths is the one actually used.
    std::vector<const char*> candidates;
    /// Required byte size, or 0 when several are legitimate.
    std::size_t size;
    /// CRC32 of a dump POM2 can vouch for, or 0 when there is no reference
    /// to check against — in that case the panel reports the checksum as
    /// identification only and passes no judgement on it.
    std::uint32_t knownCrc;
    const char* knownCrcLabel; ///< What that CRC identifies. May be empty.
    /// What POM2 does when none of the candidates resolve.
    const char* whenMissing;
};

/// The peripheral-side catalogue. Machine + character ROMs come from
/// `profileConfig()`, not from here.
inline const std::vector<RomCatalogEntry>& romCatalog()
{
    static const std::vector<RomCatalogEntry> kEntries = {
        // ─── Disk II ──────────────────────────────────────────────────────
        { "Disk II (5.25\")", "Boot PROM, 16-sector (Apple 341-0027-A)",
          { "roms/disk2.rom" }, 256, 0, "",
          "Falls back to the embedded 341-0027-A — the card still boots." },
        { "Disk II (5.25\")", "P6 sequencer PROM, 16-sector (Apple 341-0028-A)",
          { "roms/diskii_p6.rom" }, 256, 0, "",
          "Falls back to the legacy 32-cycle nibble gate. WOZ images force "
          "the bit-level LSS path anyway, using the embedded default." },
        { "Disk II (5.25\")", "Boot PROM, 13-sector (Apple 341-0009)",
          { "roms/disk2_13.rom" }, 256, 0, "",
          "13-sector (DOS 3.2 era) disks mount but cannot boot." },
        { "Disk II (5.25\")", "P6 sequencer PROM, 13-sector (Apple 341-0010)",
          { "roms/diskii_p6_13.rom" }, 256, 0, "",
          "Same: the card never switches to the 13-sector pair." },

        // ─── Storage cards ────────────────────────────────────────────────
        { "Storage cards", "Liron / SmartPort controller (4 KB)",
          { "roms/liron.rom" }, 4096, 0, "",
          "SmartPortCard presents a synthetic identity instead of the real "
          "firmware. Blocks still work; the $Cn0D dispatch does not." },
        { "Storage cards", "CFFA 2.0 firmware, 65C02 build",
          { "roms/cffa20eec02.bin" }, 4096, 0xFB3726F8u,
          "dreher.net Run6_CDROM.zip",
          "The CFFA card refuses to plug — it is a ROM-driven card." },
        { "Storage cards", "CFFA 2.0 firmware, 6502 build",
          { "roms/cffa20ee02.bin" }, 4096, 0x3ECAFCE5u,
          "dreher.net Run6_CDROM.zip",
          "Same — one of the two CFFA dumps must be present." },

        // ─── Printer / input / clock ──────────────────────────────────────
        { "Other cards", "Grappler+ parallel printer EPROM (Orange Micro)",
          { "roms/grappler_plus.bin", "roms/grappler+.bin",
            "roms/grappler.bin" }, 4096, 0, "",
          "The card plugs with a synthetic stub ROM: PR#n still prints, but "
          "software that looks for the real firmware (AppleWorks' "
          "\"Printer = Grappler+\") will not find it." },
        { "Other cards", "Mouse card slot EPROM (Apple 341-0270-C)",
          { "roms/mouse_341-0270-c.bin" }, 2048, 0, "",
          "Neither mouse card can be plugged (both variants need it)." },
        { "Other cards", "Mouse card 6805 MCU ROM (Apple 341-0269)",
          { "roms/mouse_341-0269.bin" }, 2048, 0, "",
          "The MAME-faithful mouse card refuses to plug; the AppleWin HLE "
          "variant, which only needs the slot EPROM, still works." },
        { "Other cards", "ThunderClock+ slot ROM (Thunderware)",
          { "roms/thunderclock_u9_v1.3.bin", "roms/thunderclock_u9.bin",
            "roms/thunderclock.rom" }, 0, 0, "",
          "The clock card runs its synthetic ROM — ProDOS still reads the "
          "date, but the real firmware entry points are absent." },
    };
    return kEntries;
}

}  // namespace pom2

#endif  // POM2_ROM_CATALOG_H

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// CharRomCatalog — list of character-generator ROMs the user can switch
// between at runtime via the toolbar dropdown.
//
// Apple shipped localised char ROMs for each export market (French, UK,
// German, French Canadian, …). On a Euro IIe the locale was selected by
// a physical switch under the keyboard that paged between two banks of
// the dual 8 KB EPROM; POM2 treats each locale as an independent 4 KB
// file (the "Single/" community split of the Dual-Euro dumps from
// downloads.reactivemicro.com — same content as the half of the 2764
// the switch selects).
//
// Entries with kProfileMaskII* control whether the choice appears in the
// dropdown for the active profile: a French //e ROM has no business
// being offered on a II+ profile (2 KB layout, no mousetext / lowercase).
// "Default" entries point to the profile's stock probe order so the user
// can always revert to "whatever the profile would have loaded on its
// own".

#ifndef POM2_CHAR_ROM_CATALOG_H
#define POM2_CHAR_ROM_CATALOG_H

#include "SystemProfile.h"

#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

enum class CharRomLocale : uint8_t {
    ProfileDefault = 0,    // honour the profile's charRomProbeOrder

    // 2 KB classic II / II+ font (uppercase only, no lowercase, no
    // mousetext). Kept distinct from the IIe entries because loading
    // it on a IIe profile silently disables MouseText.
    AppleIIClassic,

    // 4 KB IIe-class ROMs (lowercase + mousetext when Enhanced).
    AppleIIeUS_Enhanced,
    AppleIIeUS_Unenhanced,
    AppleIIeFrench,
    AppleIIeFrenchCanadian,
    AppleIIeFrenchCanadianUnenhanced,
    AppleIIeUK_Enhanced,
    AppleIIeUK_Unenhanced,
    AppleIIeGerman,
    AppleIIeGermanImproved,
};

struct CharRomEntry {
    CharRomLocale locale;
    const char*   displayName;
    const char*   path;             // empty when locale == ProfileDefault
    bool          isIIeClass;       // hides II/II+ entries on IIe profiles
                                    // and vice-versa (a "ProfileDefault"
                                    // entry is shown for every profile)
};

/// All entries the dropdown could ever display, in stable order.
const std::vector<CharRomEntry>& charRomCatalog();

/// True when this entry is meaningful to offer for the active profile.
/// `ProfileDefault` is universal; II / II+ get only the classic 2 KB
/// option; everything else only sees the 4 KB IIe-class entries.
bool charRomFitsProfile(const CharRomEntry& e, SystemProfile p);

/// Resolve an enum tag to its catalog entry; falls back to
/// ProfileDefault when the tag is out of range (defensive against
/// stale settings files written by a future build).
const CharRomEntry& charRomEntry(CharRomLocale l);

/// String key used in settings.cfg so the choice survives restarts.
const char* charRomLocaleKey(CharRomLocale l);
CharRomLocale charRomLocaleFromKey(const std::string& key);

/// Resolve a catalog entry's file under the same path-prefix probe the
/// rest of the boot path uses (`roms/X`, `../roms/X`, `../../roms/X`)
/// — POM2 is normally launched from `build/`, where the bare `roms/...`
/// path the catalog stores doesn't exist. Returns the first existing
/// candidate, or an empty string when the file is missing entirely.
/// Pass `ProfileDefault` to get an empty string (caller should fall
/// back to the profile probe order).
std::string resolveCharRomPath(CharRomLocale l);
std::string resolveCharRomPath(const std::string& catalogPath);

}  // namespace pom2

#endif // POM2_CHAR_ROM_CATALOG_H

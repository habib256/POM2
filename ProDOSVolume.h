// POM2 Apple II Emulator
// Copyright (C) 2026
//
// ProDOSVolume — synthesise a read-only ProDOS volume image (block array)
// from the contents of a host folder.
//
// Layout produced:
//   Block 0       Boot block (zeroed — volume is not directly bootable;
//                 the user boots ProDOS from elsewhere, then the slot 5
//                 host-folder volume appears as a secondary drive).
//   Block 1       Boot block 2 (zeroed).
//   Blocks 2-5    Volume directory (key + 3 extension blocks → 51 entries).
//   Block 6       Volume bitmap (1 block ⇒ covers up to 4096 blocks = 2 MB,
//                 plenty for typical drag-drop use).
//   Blocks 7+     File data + sapling index blocks, allocated sequentially.
//
// Scope (MVP):
//   * Flat directory only — sub-directories of the host folder are skipped.
//   * Up to 51 files (volume directory limit). Excess are skipped.
//   * Files ≤ 128 KB — supports seedling (≤ 512 B) and sapling (1 idx +
//     up to 256 data blocks). Tree files (> 128 KB) are skipped with a
//     warning. Covers virtually every Apple II program.
//   * File type guessed from host extension; defaults to BIN ($06).
//   * Aux type = 0 (no metadata to derive from).
//   * Names sanitised to ProDOS conventions (uppercased ASCII, A-Z/0-9/.,
//     starts with a letter, ≤ 15 chars; collisions get .1/.2/… suffixes).
//
// Output is a `std::vector<uint8_t>` whose size is a multiple of 512 —
// directly consumable by `ProDOSHardDiskCard::loadImageFromBytes(...)`.

#ifndef POM2_PRODOS_VOLUME_H
#define POM2_PRODOS_VOLUME_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

struct ProDOSBuildResult {
    bool        ok = false;
    std::string error;
    std::size_t filesIncluded = 0;
    std::size_t filesSkipped  = 0;     // > 128 KB, name unsanitisable, or overflow
    std::size_t totalBlocks   = 0;
};

/// Synthesise a read-only ProDOS volume from `hostFolder`. The volume's
/// header carries `volumeName` (truncated/uppercased to a ProDOS-legal
/// 15-char name; pass "HOST" for the typical case). On success,
/// `outImage` holds the volume bytes (multiple of 512). Empty / missing
/// host folders produce a valid empty volume (no files), not an error.
ProDOSBuildResult buildVolumeFromFolder(const std::string& hostFolder,
                                        const std::string& volumeName,
                                        std::vector<std::uint8_t>& outImage);

} // namespace pom2

#endif // POM2_PRODOS_VOLUME_H

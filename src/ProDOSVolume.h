// VERHILLE Arnaud 2026

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

struct ProDOSDecodeResult {
    bool        ok = false;
    std::string error;
    std::size_t filesWritten  = 0;
    std::size_t filesSkipped  = 0;
};

/// Reverse of `buildVolumeFromFolder`: walk a synthesised volume's directory
/// blocks (2..5) and write every seedling/sapling file back into `hostFolder`
/// using the inverse of the file_type → extension mapping. Files in the
/// folder that are *absent* from the volume are LEFT UNTOUCHED — never
/// deleted. Existing files whose bytes already match the volume are skipped
/// (no write, no mtime bump). Tree files (>128 KB) are skipped with a warn.
/// `hostFolder` is created if missing.
ProDOSDecodeResult decodeVolumeToFolder(const std::vector<std::uint8_t>& image,
                                        const std::string& hostFolder);

/// True iff `name` (a directory-entry name decoded from an untrusted volume
/// image) is safe to use as a single host path component. The image is
/// guest-writable RAM, so a hostile/corrupt entry can carry path separators,
/// NUL, "." / ".." — which `decodeVolumeToFolder` would otherwise join to the
/// host folder and escape the jail. A legal ProDOS name is 1..15 chars of
/// [A-Za-z0-9.] and is never "." or ".."; anything else is rejected.
bool isHostSafeProDOSName(const std::string& name);

} // namespace pom2

#endif // POM2_PRODOS_VOLUME_H

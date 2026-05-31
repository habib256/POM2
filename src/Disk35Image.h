// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Disk35Image — Sony 800K 3.5" disk image holder for the //c+ internal
// drive and the SmartPort daisy-chain port. Loads `.po`, `.2mg` and
// raw 819 200-byte images and exposes a flat block-array view
// (`getBlock(idx) -> 512 bytes`, 1600 blocks total) to the
// `Sony35Drive` consumer.
//
// Phase 1 scope (this file): block storage + 2IMG header support. The
// Sony zoned GCR encoding (5 zones × 16 tracks, 12/11/10/9/8 sectors
// per track, 4:4 GCR) that the IWM bit-cell walker needs to clock out
// is Phase 2 — when that lands, `expandTrackBits(qt)` will be added
// alongside the existing 5.25" path in `DiskImage`. We keep this file
// separate from `DiskImage` because the on-disk physics is different
// enough (variable-rate flux, 80 tracks × 2 sides, no quarter-track
// stepping) that fusing them would tangle two state machines.
//
// MAME source-of-truth references (for the Phase 2 encoder):
//   * `src/lib/formats/ap_dsk35.cpp`     — block ↔ GCR sector encoder
//   * `src/devices/imagedev/floppy.cpp`  — 3.5" zone constants
//   * `src/devices/machine/applefdintf.cpp::add_35`
//
// The image is read/write under user opt-in via `setWriteBackEnabled`.
// Write-back rewrites the .po payload (preserving the 2IMG envelope).

#ifndef POM2_DISK35_IMAGE_H
#define POM2_DISK35_IMAGE_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class Disk35Image
{
public:
    /// 800K 3.5" geometry: 80 tracks × 2 sides × 10 avg sectors × 512 B
    /// = 819 200 bytes. Per-track sector count is zoned (12/11/10/9/8)
    /// for variable-rate Sony recording. Phase 1 stores the flat
    /// payload only; the zone schedule lives in `kSectorsForTrack`
    /// for the Phase 2 encoder.
    static constexpr int      kTracks       = 80;
    static constexpr int      kSides        = 2;
    static constexpr int      kBlockBytes   = 512;
    static constexpr int      kBlockCount   = 1600;
    static constexpr uint32_t kBytesPerImage = kBlockCount * kBlockBytes; // 819200

    /// MAME `ap_dsk35.cpp:apple35_sectors_per_track[]` — count of
    /// physical sectors on the given 3.5" track number (0..79). The 5
    /// zones run 0..15 / 16..31 / 32..47 / 48..63 / 64..79.
    static int sectorsForTrack(int track);

    enum class ImageKind {
        Unknown,
        Raw800k,       // bare 819 200-byte payload (.po, .dsk-as-prodos)
        TwoImg800k,    // .2mg with 2IMG header wrapping a 819 200 payload
    };

    Disk35Image() = default;

    /// Load a 3.5" image. Currently accepts:
    ///  * 819 200-byte raw images (assumed ProDOS block order)
    ///  * 2IMG-wrapped 819 200-byte ProDOS images
    /// On failure, returns false and populates `lastError`.
    bool loadFile(const std::string& path);

    /// Discard the loaded image.
    void eject();

    bool        isLoaded()         const { return loaded_; }
    const std::string& path()      const { return path_; }
    const std::string& lastError() const { return lastError_; }
    ImageKind   kind()             const { return kind_; }

    /// Read 512 bytes from block `idx` (0..1599). Returns true on
    /// success. Out-of-range or no-image-loaded → false (out is
    /// untouched).
    bool readBlock (uint32_t idx, uint8_t out[kBlockBytes]) const;

    /// Write 512 bytes to block `idx`. No-op (returns false) if no
    /// image is loaded, the block index is out of range, or write-back
    /// is disabled. Mark the image dirty for the next `saveDirty()`.
    bool writeBlock(uint32_t idx, const uint8_t in[kBlockBytes]);

    /// True if any block has been written since the last load/save.
    bool hasUnsavedChanges() const { return dirty_; }

    /// Flush dirty blocks back to the source file. Re-emits the 2IMG
    /// envelope if one was present at load time. Returns true on
    /// success. After save, `dirty_` is cleared.
    bool saveDirty();

    bool isWriteProtected() const {
        return fileWriteProtected_ || !writeBackEnabled_;
    }
    void setWriteBackEnabled(bool on) { writeBackEnabled_ = on; }
    bool isWriteBackEnabled() const   { return writeBackEnabled_; }

private:
    bool         loaded_              = false;
    bool         dirty_               = false;
    bool         writeBackEnabled_    = false;
    bool         fileWriteProtected_  = false;
    ImageKind    kind_                = ImageKind::Unknown;
    std::string  path_;
    std::string  lastError_;

    // Flat block-major payload (1600 × 512 = 819 200 bytes). Heap-
    // allocated because file-scope members > 800 KB would push the
    // class out of the typical cache-line-friendly small-object range
    // for the controller wiring.
    std::vector<uint8_t> blocks_;

    // Captured 2IMG header verbatim so saveDirty re-emits a valid
    // wrapper. Empty when the source was a raw `.po` / `.dsk`.
    std::vector<uint8_t> twoImgHeaderRaw_;
    std::vector<uint8_t> twoImgTrailerRaw_;
};

}  // namespace pom2

#endif // POM2_DISK35_IMAGE_H

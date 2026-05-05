// POM2 Apple II Emulator
// Copyright (C) 2026
//
// DiskImage — loads a 143 360-byte Apple II floppy image (.dsk / .do,
// DOS-3.3 logical sector order) and pre-nibblizes it into per-track
// buffers ready for the Disk II controller to clock out.
//
// On-disk layout per sector (~382 nibbles), repeated 16 times per track:
//
//   14 × $FF              sync gap
//   $D5 $AA $96           address-field prologue
//   8 × 4-and-4 nibbles   volume / track / sector / checksum
//   $DE $AA $EB           address-field epilogue
//   5 × $FF               inter-field sync gap
//   $D5 $AA $AD           data-field prologue
//   343 × 6-and-2 nibble  256 data bytes + final XOR checksum
//   $DE $AA $EB           data-field epilogue
//
// 6-and-2 packing (per "Beneath Apple DOS"): each input byte is split into
// the top 6 bits (256 "high" entries) and bottom 2 bits. The 86 "low" entries
// hold three bit-pairs each, packed with the pair-bits swapped for clean
// running-XOR checksum behaviour. On disk the 86 low nibbles are written in
// REVERSE order, then the 256 high nibbles in normal order, then one final
// checksum nibble. All output is run through a 64-entry GCR translate table
// that maps 6-bit values to valid disk bytes (always bit-7 set, no run of
// two zero bits — the constraints the Disk II's data-separator can recover).
//
// Read-only for now: the buffer is regenerated only on insert.

#ifndef POM2_DISK_IMAGE_H
#define POM2_DISK_IMAGE_H

#include <array>
#include <cstdint>
#include <string>

class DiskImage
{
public:
    static constexpr int kTracks            = 35;
    static constexpr int kSectorsPerTrack   = 16;
    static constexpr int kSectorBytes       = 256;
    static constexpr int kBytesPerImage     = kTracks * kSectorsPerTrack * kSectorBytes; // 143360
    static constexpr int kNibblesPerTrack   = 6656;

    DiskImage();

    /// Load a .dsk / .do image (DOS 3.3 logical sector order). Returns
    /// true on success. On failure `getLastError()` has details. Mounting
    /// a new image discards any previously loaded buffer.
    bool loadFile(const std::string& path);

    /// Discard the loaded image. After eject, isLoaded() returns false
    /// and the controller will see the same "no media" behaviour as if
    /// the image had never been loaded.
    void eject();

    bool        isLoaded()       const { return loaded; }
    const std::string& getPath() const { return path; }
    const std::string& getLastError() const { return lastError; }

    /// Read one nibble from `track` at buffer index `index`. `index` is
    /// wrapped modulo kNibblesPerTrack so the controller can advance the
    /// head cursor without bothering to wrap. Returns $FF (= sync nibble)
    /// when no image is loaded or the track is out of range.
    uint8_t nibbleAt(int track, int index) const;

    /// We only ship read-only support for now; the bit reflects what the
    /// controller's $C0nD-in-read-mode probe should return (write-protect
    /// = bit 7 set).
    bool isWriteProtected() const { return true; }

private:
    bool loaded = false;
    std::string path;
    std::string lastError;

    // 35 × 6656 = ~228 KB. Heap allocation would also work but a flat
    // member fits the "one concern, plain data" style of POM2.
    using TrackBuffer = std::array<uint8_t, kNibblesPerTrack>;
    std::array<TrackBuffer, kTracks> tracks;

    void nibblizeTrack(int track, const uint8_t* sectors, uint8_t volume);
    static void writeAddressField(uint8_t*& dst, uint8_t volume,
                                  uint8_t track, uint8_t sector);
    static void writeDataField   (uint8_t*& dst, const uint8_t* src);
};

#endif // POM2_DISK_IMAGE_H

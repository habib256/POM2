// POM2 Apple II Emulator
// Copyright (C) 2026
//
// DiskImage — loads a 143 360-byte Apple II floppy image (.dsk / .do
// in DOS 3.3 logical sector order, or .po in ProDOS sector order) and
// pre-nibblizes it into per-track buffers ready for the Disk II
// controller to clock out. The on-disk physical sector layout is the
// same in both cases (16 sectors per track, GCR-encoded); the file
// format only changes which file offset maps to which physical sector
// via the skew table.
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
#include <vector>

class DiskImage
{
public:
    static constexpr int kTracks            = 35;
    static constexpr int kSectorsPerTrack   = 16;
    static constexpr int kSectorBytes       = 256;
    static constexpr int kBytesPerImage     = kTracks * kSectorsPerTrack * kSectorBytes; // 143360
    static constexpr int kNibblesPerTrack   = 6656;

    /// File-format sector ordering. Both produce the same on-disk physical
    /// sector layout — only the skew between file offset and physical
    /// sector P differs.
    enum class SectorOrder { Dos33, ProDOS };

    DiskImage();

    /// Load a .dsk / .do image (DOS 3.3 logical sector order), a .po
    /// image (ProDOS sector order), a .nib raw nibble stream (35 × 6656
    /// bytes, no encoding/decoding), or a .woz bit-cell image
    /// (Apple //e copy-protected disks; verbatim port of MAME's
    /// `src/lib/formats/woz_dsk.cpp` chunk parser). When `order` is
    /// omitted, falls back to extension sniffing: `.po` → ProDOS,
    /// `.nib` → raw, `.woz` → WOZ1/WOZ2, anything else → DOS 3.3.
    /// Returns true on success. On failure `getLastError()` has details.
    /// Mounting a new image discards any previously loaded buffer.
    bool loadFile(const std::string& path);
    bool loadFile(const std::string& path, SectorOrder order);
    SectorOrder getSectorOrder() const { return sectorOrder; }
    bool isNib() const { return nibFormat; }
    /// True iff the image was loaded from a .woz file. WOZ images are
    /// bit-cell-native: the legacy 32-cycle nibble gate cannot decode
    /// them; DiskIICard forces the LSS path on insert. Always reported
    /// write-protected for now (write-back to .woz is not yet
    /// implemented — incoming flux events get dropped).
    bool isWoz() const { return wozFormat; }

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

    /// Write one nibble at `track[index]`. Marks the track dirty so a
    /// subsequent saveDirty() will persist it back to the source file.
    /// No-op when no image is loaded or the track is out of range.
    void writeNibbleAt(int track, int index, uint8_t value);

    // ── LSS bit-cell stream ─────────────────────────────────────────────
    //
    // The Disk II's Logic State Sequencer reads bit cells, not nibbles —
    // each cell is 4 µs on real hardware. Sync $FF bytes carry two
    // trailing zero cells (10 cells total) so the byte boundary slips by
    // 2 bits per sync gap; this is what lets the LSS *lose* alignment
    // during a sync run and *re-sync* on the next data prologue's leading
    // 1-bit. Without this padding, a soft-LSS that always emits 8 cells
    // per byte stays aligned forever — DOS / ProDOS RWTS still works
    // (they probe the latch one byte at a time) but Copy II Plus's RWTS
    // and any copy-protection that bit-bangs $C0EC fails.
    //
    // Standard .dsk/.do/.po expansion (per MAME `ap2_dsk.cpp`):
    //   • each $FF in a run of 2+ contiguous $FFs → 10 cells
    //   • lone $FF                                 → 8 cells
    //   • everything else                          → 8 cells
    // .nib raw nibble images have no sync semantics — every byte = 8 cells.

    /// Total bit-cell count for `track`. Standard 16-sector .dsk/.do/.po:
    /// ~54944 cells (varies a few hundred either way depending on tail
    /// padding). .nib raw images: exactly 53248 cells (= 6656 × 8).
    int trackBitLength(int track) const;

    /// Read one bit cell (0 or 1) from `track[bitIdx]`. `bitIdx` wraps
    /// modulo trackBitLength(track). First call per (image, track) lazily
    /// expands the bit-cell cache; subsequent reads are O(1) array index.
    /// `writeNibbleAt` invalidates that track's cache.
    uint8_t bitAt(int track, int bitIdx) const;

    // ── MAME-style flux event view ──────────────────────────────────────
    //
    // Verbatim port of MAME's `floppy_image_device` flux model. A track is
    // a sorted list of flux transition timestamps. Each transition is one
    // physical flux orientation flip on the magnetic surface; the LSS's
    // PULSE input goes high for one LSS cycle when the head crosses one.
    //
    // Time unit: 1 LSS cycle = 0.5 µs (2 per CPU cycle). Per revolution =
    // `trackPeriod(track)` LSS cycles. For a stock 16-sector .dsk that's
    // ~440k LSS cycles (≈ 215 ms ≈ 280 RPM — slightly slow vs MAME's
    // 200 ms / 300 RPM but consistent across reads/writes within POM2).
    //
    // The flux array is derived lazily from the nibble buffer: each "1"
    // bit cell becomes one flux event at the cell center. Per-cell layout
    // matches the bit-cell stream above (8 cells/byte, 10 cells per $FF
    // in a sync run), so reads through the flux model and reads through
    // the bit-cell view produce identical PULSE timing for the same
    // nibble buffer. Cache invalidates on `writeNibbleAt` and `eject`.

    /// Per-track period in LSS cycles. Equal to `trackBitLength(track) * 8`.
    int trackPeriod(int track) const;

    /// Sorted vector of flux event timestamps (LSS cycles, in [0, period)).
    /// Returns a static empty vector for out-of-range tracks. Lazy-built
    /// on first call per (image, track); reused across calls until the
    /// underlying nibble buffer changes.
    const std::vector<int>& fluxEvents(int track) const;

    /// MAME `floppy_image_device::get_next_transition(from_when)` —
    /// returns the LSS-cycle timestamp of the next flux transition at or
    /// after `fromLssCycle`. Wraps across revolution boundaries: if the
    /// current revolution has no further events past `fromLssCycle`, the
    /// returned time is in the next revolution (so result ≥ fromLssCycle
    /// always). Returns `kFluxNever` only if the track has zero events.
    static constexpr int64_t kFluxNever = INT64_MAX;
    int64_t getNextTransition(int track, int64_t fromLssCycle) const;

    /// MAME `floppy_image_device::write_flux(start, end, count, transitions)`
    /// — splice a window of flux events into `track`, replacing whatever
    /// was previously in that LSS-cycle range. Re-derives the affected
    /// span of the nibble buffer (cell-windowed flux→bit conversion, then
    /// 8-bit packing into nibble cells starting at the cell containing
    /// `startLssCycle`) so save-back, decode, and bit-cell read-back all
    /// see the new contents. Marks the track dirty.
    void writeFlux(int track, int64_t startLssCycle, int64_t endLssCycle,
                   int count, const int64_t* transitions);

    /// MAME `floppy_image_device::set_write_splice(when)` — informational
    /// hook for the upcoming IWM port; currently a no-op for the Disk II
    /// path because the splice point is implicit in `writeFlux`.
    void setWriteSplice(int /*track*/, int64_t /*lssCycle*/) {}

    /// True if any track has been written since load. Cleared by
    /// saveDirty() and load.
    bool hasUnsavedChanges() const { return anyDirty; }

    /// Decode each dirty track back to the source-file format (.dsk/
    /// .do/.po: 16 logical sectors × 256 bytes per track; .nib: raw
    /// nibble buffer) and overwrite the source file. Returns true on
    /// success. On failure `getLastError()` has details. After save
    /// the dirty bits are cleared.
    bool saveDirty();

    /// User opt-in for write-back. Default: false (read-only) to avoid
    /// silently mutating the source file. Mainwindow flips this before
    /// eject if the user has opted in. WOZ images stay write-protected
    /// regardless of this flag — first-cut WOZ support is read-only.
    bool isWriteProtected() const { return wozFormat || !writeBackEnabled; }
    void setWriteBackEnabled(bool on) { writeBackEnabled = on; }

private:
    bool loaded = false;
    SectorOrder sectorOrder = SectorOrder::Dos33;
    bool nibFormat = false;
    /// Set when loadFile parses a .woz successfully. WOZ stores bit cells
    /// directly so loadWoz populates `bitStream[track]` instead of the
    /// nibble buffer; the existing `expandTrackFlux` derives flux events
    /// from the bit stream as usual. Persisted writes (writeFlux) are
    /// suppressed for WOZ — the splice would clobber the canonical bit
    /// data. Cleared on eject() / non-WOZ load.
    bool wozFormat = false;
    std::string path;
    std::string lastError;
    bool writeBackEnabled = false;
    bool anyDirty = false;

    // 35 × 6656 = ~228 KB. Heap allocation would also work but a flat
    // member fits the "one concern, plain data" style of POM2.
    using TrackBuffer = std::array<uint8_t, kNibblesPerTrack>;
    std::array<TrackBuffer, kTracks> tracks;
    std::array<bool, kTracks>        dirty{};

    // Lazy per-track bit-cell expansion cache. Populated on first
    // `bitAt`/`trackBitLength` call; invalidated by `writeNibbleAt` /
    // `eject` / new `loadFile`. Mutable so that const `bitAt` can fill
    // the cache on demand.
    mutable std::array<std::vector<uint8_t>, kTracks> bitStream;
    mutable std::array<bool, kTracks>                 bitStreamValid{};
    void expandTrackBits(int track) const;

    // Lazy per-track flux event cache (sorted ascending, in [0, period)).
    // Mirrors MAME's m_image flux-event vector but stored in LSS cycles
    // rather than 200M-position units.
    mutable std::array<std::vector<int>, kTracks> fluxStream;
    mutable std::array<bool, kTracks>             fluxStreamValid{};
    void expandTrackFlux(int track) const;

    void invalidateBitStream(int track) {
        bitStreamValid[track]  = false;
        fluxStreamValid[track] = false;
        bitStream[track].clear();
        fluxStream[track].clear();
    }
    void invalidateAllBitStreams() {
        bitStreamValid.fill(false);
        fluxStreamValid.fill(false);
        for (auto& bs : bitStream)  bs.clear();
        for (auto& fs : fluxStream) fs.clear();
    }

    void nibblizeTrack(int track, const uint8_t* sectors, uint8_t volume,
                       const int* logicalForPhysical);
    static void writeAddressField(uint8_t*& dst, uint8_t volume,
                                  uint8_t track, uint8_t sector);
    static void writeDataField   (uint8_t*& dst, const uint8_t* src);

    /// WOZ1/WOZ2 parser. Verbatim follower of MAME's
    /// `src/lib/formats/woz_dsk.cpp` chunk walk: looks for INFO, TMAP,
    /// TRKS in any order; accepts both WOZ1 (160 fixed 6656-byte slots,
    /// bit_count at offset +6648 LE u16) and WOZ2 (160 × 8-byte TRK
    /// headers; data at file offset starting_block × 512 with
    /// bit_count as u32). Bits are MSB-first within each byte. Each
    /// physical track 0..34 sources its bit stream from
    /// TMAP[track*4] (the canonical quarter-track slot at the centre
    /// of the track — same convention as MAME's `cell_data_index`).
    /// Quarter-track sub-positions used by some copy protections are
    /// not yet preserved; this is the smallest patch that lets
    /// stock-protection .woz disks boot.
    bool loadWoz(const std::string& imgPath);

    /// Decode one track's nibble buffer back into 16 × 256-byte logical
    /// sectors. Returns true if any sector was successfully decoded.
    /// Sectors that can't be parsed (no prologue, bad checksum) are
    /// left untouched in `outSectors` (which the caller pre-fills with
    /// the existing file content so unmodified sectors persist).
    bool decodeTrack(int track, uint8_t outSectors[kSectorsPerTrack][kSectorBytes]) const;
};

#endif // POM2_DISK_IMAGE_H

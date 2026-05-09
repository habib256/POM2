// POM2 Apple II Emulator
// Copyright (C) 2026

#include "DiskImage.h"
#include "Logger.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

// 6-bit → on-disk nibble translation table. The 64 valid disk byte values:
// bit-7 always set, no two consecutive zero bits, and no zero run that
// the Disk II's analog data separator can't recover.
constexpr uint8_t kGcrTable[64] = {
    0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
    0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
    0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
    0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
    0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
    0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
    0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
    0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

// DOS 3.3 sector skewing: physical sector P on disk holds the data for
// logical sector kDos33LogicalForPhysical[P]. The .dsk file stores
// sectors in *logical* order (sector 0 first); on disk we write them in
// *physical* order, pulling each slot's data via this mapping.
constexpr int kDos33LogicalForPhysical[16] = {
    0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15
};

// ProDOS sector skewing for 5.25" disks. Mirror skew of the DOS 3.3 table
// — both are constant-skew variants of the standard +7/-8 Apple
// interleave, with sectors 0 and 15 fixed. Used for .po images, which
// store data in ProDOS-logical-sector order.
constexpr int kProDosLogicalForPhysical[16] = {
    0, 8, 1, 9, 2, 10, 3, 11, 4, 12, 5, 13, 6, 14, 7, 15
};

bool endsWithCi(const std::string& s, const char* suffix)
{
    const size_t n = std::strlen(suffix);
    if (s.size() < n) return false;
    for (size_t i = 0; i < n; ++i) {
        const char a = s[s.size() - n + i];
        const char b = suffix[i];
        const auto lc = [](char c) -> char {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
        };
        if (lc(a) != lc(b)) return false;
    }
    return true;
}

// 4-and-4 encoding: split a byte into "odd" (high) and "even" (low) bit
// halves, OR each with $AA so the result is always a valid disk nibble
// (bit-7 set, alternating bits guarantee no zero runs).
inline void write4and4(uint8_t*& dst, uint8_t b)
{
    *dst++ = static_cast<uint8_t>(((b >> 1) & 0x55) | 0xAA);
    *dst++ = static_cast<uint8_t>((b & 0x55) | 0xAA);
}

// Bit-reverse a 2-bit pair: bit 0 ↔ bit 1. Used when packing the low-2-bit
// triples for the 86 secondary nibbles — the swap makes the running-XOR
// checksum recover the original byte cleanly on read-back.
inline uint8_t rev2(uint8_t b) { return ((b & 1) << 1) | ((b >> 1) & 1); }

// Inverse of kGcrTable: maps a disk byte ($96-$FF) back to its 6-bit
// value (0..63), or 0xFF if the byte isn't a valid GCR nibble.
constexpr std::array<uint8_t, 256> makeGcrInverse()
{
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) t[i] = 0xFF;
    for (uint8_t v = 0; v < 64; ++v) t[kGcrTable[v]] = v;
    return t;
}
constexpr std::array<uint8_t, 256> kGcrInverse = makeGcrInverse();

// Decode one 4-and-4 byte (8 nibbles → 4 bytes). Returns the byte; the
// caller advances the cursor by 2.
inline uint8_t decode4and4(const uint8_t* p)
{
    return static_cast<uint8_t>(((p[0] << 1) & 0xAA) | (p[1] & 0x55));
}

}  // namespace

DiskImage::DiskImage()
{
    for (auto& t : tracks) t.fill(0xFF);
}

uint8_t DiskImage::nibbleAt(int track, int index) const
{
    if (!loaded || track < 0 || track >= kTracks) return 0xFF;
    const int n = ((index % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack;
    return tracks[track][n];
}

bool DiskImage::loadFile(const std::string& imgPath)
{
    // Sniff the extension:
    //   .woz → WOZ1 / WOZ2 bit-cell image (MAME woz_dsk.cpp port)
    //   .nib → raw nibble stream (no encoding)
    //   .po  → ProDOS sector order
    //   else → DOS 3.3 sector order (.dsk, .do, or no extension)
    if (endsWithCi(imgPath, ".woz")) {
        return loadWoz(imgPath);
    }
    if (endsWithCi(imgPath, ".nib")) {
        std::ifstream f(imgPath, std::ios::binary);
        if (!f) {
            lastError = "Cannot open " + imgPath;
            loaded = false;
            return false;
        }
        f.seekg(0, std::ios::end);
        const auto size = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        const size_t expected = static_cast<size_t>(kTracks) * kNibblesPerTrack;
        if (size != expected) {
            lastError = "Expected " + std::to_string(expected) +
                        "-byte .nib image, got " + std::to_string(size);
            loaded = false;
            return false;
        }
        for (int t = 0; t < kTracks; ++t) {
            f.read(reinterpret_cast<char*>(tracks[t].data()), kNibblesPerTrack);
        }
        if (!f) {
            lastError = "Short read on " + imgPath;
            loaded = false;
            return false;
        }
        path        = imgPath;
        loaded      = true;
        nibFormat   = true;
        wozFormat   = false;
        sectorOrder = SectorOrder::Dos33;     // not meaningful for .nib
        dirty.fill(false);
        anyDirty    = false;
        lastError.clear();
        invalidateAllBitStreams();
        pom2::log().info("Disk II", "Loaded " + imgPath +
                         " (.nib raw nibble stream, 35 tracks)");
        return true;
    }

    const SectorOrder order = endsWithCi(imgPath, ".po")
                              ? SectorOrder::ProDOS
                              : SectorOrder::Dos33;
    return loadFile(imgPath, order);
}

bool DiskImage::loadFile(const std::string& imgPath, SectorOrder order)
{
    std::ifstream f(imgPath, std::ios::binary);
    if (!f) {
        lastError = "Cannot open " + imgPath;
        loaded = false;
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != kBytesPerImage) {
        lastError = "Expected " + std::to_string(kBytesPerImage) +
                    "-byte 5.25\" image, got " + std::to_string(size);
        loaded = false;
        return false;
    }

    std::vector<uint8_t> buf(size);
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size));
    if (!f) {
        lastError = "Short read";
        loaded = false;
        return false;
    }

    constexpr uint8_t kVolume = 254;     // DOS 3.3 default volume
    const int* skew = (order == SectorOrder::ProDOS)
                      ? kProDosLogicalForPhysical
                      : kDos33LogicalForPhysical;
    for (int t = 0; t < kTracks; ++t) {
        nibblizeTrack(t,
            buf.data() + t * kSectorsPerTrack * kSectorBytes,
            kVolume, skew);
    }

    path        = imgPath;
    loaded      = true;
    nibFormat   = false;
    wozFormat   = false;
    sectorOrder = order;
    dirty.fill(false);
    anyDirty    = false;
    lastError.clear();
    invalidateAllBitStreams();
    pom2::log().info("Disk II", "Loaded " + imgPath +
                     " (35 tracks, 16 sectors, GCR-encoded, " +
                     (order == SectorOrder::ProDOS ? "ProDOS" : "DOS 3.3") +
                     " order)");
    return true;
}

// ── WOZ1 / WOZ2 loader ─────────────────────────────────────────────────────
//
// Verbatim follower of MAME `src/lib/formats/woz_dsk.cpp`. The WOZ format
// stores raw bit cells (the LSS's natural input) instead of nibbles or
// sectors; that's why .woz disks survive copy protections that tweak
// inter-byte timing or bit alignment, where re-encoded .dsk synthesises
// idealised GCR and loses the protection signature.
//
// File layout (both versions):
//   12-byte header: magic "WOZ1\xFF\n\r\n" or "WOZ2\xFF\n\r\n"
//                  + 4-byte CRC32 of the rest of the file (LE; we skip
//                    validation — corrupt CRC is rare in practice and
//                    MAME's behaviour is to load anyway, log a warning).
//   chunks: 4-byte chunk_id + 4-byte LE length + payload, repeated.
//           Mandatory: INFO, TMAP, TRKS. Optional: META, WRIT, FLUX.
//
// INFO (>= 60 bytes):
//   [0]  info_version  (1 = WOZ1 style INFO, 2+ = WOZ2 fields)
//   [1]  disk_type     (1 = 5.25", 2 = 3.5") — POM2 only handles 5.25"
//   [2]  write_protected
//   [3]  synchronized
//   [4]  cleaned
//   [5..36]  creator (32 chars, space-padded)
//   v2+: disk_sides, boot_sector_format, optimal_bit_timing,
//        compatible_hardware (LE u16), required_ram (LE u16),
//        largest_track (LE u16, in 512-byte blocks)
//
// TMAP (160 bytes):
//   One byte per quarter-track 0..159. Value = TRK index (0..159) or
//   $FF when that quarter-track is unused. Multiple quarter-tracks
//   typically share a TRK (e.g. TMAP[0..2] = 0, TMAP[3] = $FF for the
//   classic "track 0 covers 3 of 4 quarter-track positions").
//
// TRKS:
//   WOZ1: 160 fixed-size 6656-byte slots.
//     bytes 0..6645  bit data (MSB-first within each byte)
//     6646..6647     bytes_used    (LE u16)
//     6648..6649     bit_count     (LE u16)
//     6650..6651     splice_point  (LE u16, $FFFF = none)
//     6652           splice_nibble
//     6653           splice_bit_count
//   WOZ2: 160 × 8-byte TRK headers, then track bit data:
//     hdr  starting_block (LE u16, 0 = unused; offset = block × 512)
//          block_count    (LE u16)
//          bit_count      (LE u32)
//
// POM2 only resolves whole tracks (35), pulling each from TMAP[track*4]
// — the canonical "centre of the track" quarter-track slot. Distinct
// per-quarter-track bit data (used by some advanced protections like
// David-DOS or Locksmith) is collapsed; that's a deliberate first-cut
// shortcut. Once we extend DiskIICard's head-position interface to
// quarter-track resolution, this can be lifted.
bool DiskImage::loadWoz(const std::string& imgPath)
{
    std::ifstream f(imgPath, std::ios::binary);
    if (!f) {
        lastError = "Cannot open " + imgPath;
        loaded = false; return false;
    }
    f.seekg(0, std::ios::end);
    const auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (fileSize < 12) {
        lastError = "WOZ file truncated (" + std::to_string(fileSize) + " bytes)";
        loaded = false; return false;
    }
    std::vector<uint8_t> buf(fileSize);
    f.read(reinterpret_cast<char*>(buf.data()),
           static_cast<std::streamsize>(fileSize));
    if (!f) {
        lastError = "Short read on " + imgPath;
        loaded = false; return false;
    }

    const bool isWoz1 = std::memcmp(buf.data(), "WOZ1", 4) == 0;
    const bool isWoz2 = std::memcmp(buf.data(), "WOZ2", 4) == 0;
    if (!isWoz1 && !isWoz2) {
        lastError = "Not a WOZ file (missing WOZ1/WOZ2 magic)";
        loaded = false; return false;
    }
    if (buf[4] != 0xFF || buf[5] != 0x0A
        || buf[6] != 0x0D || buf[7] != 0x0A) {
        lastError = "WOZ header sentinel bytes wrong";
        loaded = false; return false;
    }
    // bytes 8..11 = CRC32 of the rest of the file. We follow MAME's
    // permissive policy and don't fail on mismatch (some archived .woz
    // files have stale CRCs). Future enhancement: recompute and warn.

    // Walk chunks starting at offset 12.
    int      diskType = 1;
    bool     fileWriteProtected = false;
    int      infoVersion = isWoz2 ? 2 : 1;
    bool     haveInfo = false;
    bool     haveTmap = false;
    std::array<uint8_t, 160> tmap{};
    tmap.fill(0xFF);
    size_t   trksOff = 0;
    size_t   trksLen = 0;

    auto readU16LE = [&](size_t o) -> uint32_t {
        return static_cast<uint32_t>(buf[o])
             | (static_cast<uint32_t>(buf[o + 1]) << 8);
    };
    auto readU32LE = [&](size_t o) -> uint32_t {
        return static_cast<uint32_t>(buf[o])
             | (static_cast<uint32_t>(buf[o + 1]) << 8)
             | (static_cast<uint32_t>(buf[o + 2]) << 16)
             | (static_cast<uint32_t>(buf[o + 3]) << 24);
    };

    size_t off = 12;
    while (off + 8 <= fileSize) {
        const uint32_t len = readU32LE(off + 4);
        const size_t   dataOff = off + 8;
        if (dataOff + len > fileSize) {
            lastError = "WOZ chunk length runs past EOF";
            loaded = false; return false;
        }
        if (std::memcmp(buf.data() + off, "INFO", 4) == 0) {
            if (len >= 5) {
                infoVersion        = buf[dataOff + 0];
                diskType           = buf[dataOff + 1];
                fileWriteProtected = buf[dataOff + 2] != 0;
            }
            haveInfo = true;
        } else if (std::memcmp(buf.data() + off, "TMAP", 4) == 0) {
            if (len >= 160) {
                std::memcpy(tmap.data(), buf.data() + dataOff, 160);
                haveTmap = true;
            }
        } else if (std::memcmp(buf.data() + off, "TRKS", 4) == 0) {
            trksOff = dataOff;
            trksLen = len;
        }
        // META / WRIT / FLUX / unknown: ignored (matches MAME's
        // "load anyway" policy for forward-compatible chunks).
        off = dataOff + len;
    }

    if (!haveInfo || !haveTmap || trksLen == 0) {
        lastError = "WOZ file missing INFO/TMAP/TRKS";
        loaded = false; return false;
    }
    if (diskType != 1) {
        lastError = "WOZ disk_type " + std::to_string(diskType)
                  + " not supported (only 5.25\" / type 1)";
        loaded = false; return false;
    }

    // Reset state. Tracks[] is filled with sync $FF — the legacy gate
    // returns endless sync (won't boot, but won't crash) if someone
    // accidentally bypasses the LSS path on a WOZ image.
    for (auto& t : tracks) t.fill(0xFF);
    invalidateAllBitStreams();
    dirty.fill(false);
    anyDirty = false;

    // Per-whole-track bit-stream unpack.
    int populatedTracks = 0;
    for (int t = 0; t < kTracks; ++t) {
        const uint8_t trkIdx = tmap[t * 4];
        if (trkIdx == 0xFF) continue;     // track absent

        size_t bitDataOff   = 0;
        size_t bitDataBytes = 0;
        size_t bitCount     = 0;
        if (isWoz1) {
            // WOZ1 fixed-slot layout. Each TRK is exactly 6656 bytes.
            const size_t slotOff = trksOff + static_cast<size_t>(trkIdx) * 6656;
            if (slotOff + 6656 > trksOff + trksLen) continue;
            bitDataOff   = slotOff;
            bitDataBytes = 6646;
            bitCount     = readU16LE(slotOff + 6648);
        } else {
            // WOZ2: 8-byte TRK headers at the start of TRKS, data at
            // file-absolute block offsets.
            const size_t hdrOff = trksOff + static_cast<size_t>(trkIdx) * 8;
            if (hdrOff + 8 > trksOff + trksLen) continue;
            const uint32_t startBlock = readU16LE(hdrOff + 0);
            const uint32_t blockCount = readU16LE(hdrOff + 2);
            const uint32_t bc         = readU32LE(hdrOff + 4);
            if (startBlock == 0 || blockCount == 0 || bc == 0) continue;
            bitDataOff   = static_cast<size_t>(startBlock) * 512;
            bitDataBytes = static_cast<size_t>(blockCount) * 512;
            bitCount     = bc;
        }
        if (bitCount == 0
            || bitCount > bitDataBytes * 8
            || bitDataOff + bitDataBytes > fileSize) {
            // Defensive: skip malformed track rather than aborting load.
            continue;
        }

        auto& bits = bitStream[t];
        bits.resize(bitCount);
        for (size_t b = 0; b < bitCount; ++b) {
            const size_t byteIdx   = b / 8;
            const int    bitInByte = 7 - static_cast<int>(b % 8);
            bits[b] = static_cast<uint8_t>(
                (buf[bitDataOff + byteIdx] >> bitInByte) & 1);
        }
        bitStreamValid[t] = true;
        ++populatedTracks;
    }

    if (populatedTracks == 0) {
        lastError = "WOZ file has no usable whole tracks";
        loaded = false; return false;
    }

    path        = imgPath;
    loaded      = true;
    nibFormat   = false;
    wozFormat   = true;
    sectorOrder = SectorOrder::Dos33;     // not meaningful for .woz
    lastError.clear();
    // fileWriteProtected is informational — isWriteProtected() always
    // returns true for WOZ regardless. Logged so the user sees it.
    pom2::log().info("Disk II",
        std::string("Loaded ") + imgPath + " (.woz "
        + (isWoz2 ? "v2" : "v1")
        + ", info_v" + std::to_string(infoVersion)
        + ", " + std::to_string(populatedTracks) + " tracks"
        + (fileWriteProtected ? ", file-WP" : "")
        + ")");
    return true;
}

void DiskImage::eject()
{
    loaded = false;
    nibFormat = false;
    wozFormat = false;
    path.clear();
    for (auto& t : tracks) t.fill(0xFF);
    dirty.fill(false);
    anyDirty = false;
    invalidateAllBitStreams();
}

void DiskImage::writeNibbleAt(int track, int index, uint8_t value)
{
    if (!loaded || track < 0 || track >= kTracks) return;
    const int n = ((index % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack;
    if (tracks[track][n] != value) {
        tracks[track][n] = value;
        dirty[track]     = true;
        anyDirty         = true;
        // Bit-cell cache for this track is now stale; next bitAt() call
        // will rebuild it from the new nibble buffer.
        invalidateBitStream(track);
    }
}

// ── LSS bit-cell stream expansion ────────────────────────────────────────
//
// Walks the 6656-byte nibble buffer once and emits the LSS-shaped bit
// stream into bitStream[track]. Sync $FF runs get 2 zero cells appended
// per byte so the byte boundary drifts +2 bits across each gap — that's
// the timing artefact real Disk II software uses to recover sync after
// the head crosses a track boundary or the controller drops alignment.
void DiskImage::expandTrackBits(int track) const
{
    auto& bits = bitStream[track];
    bits.clear();
    if (track < 0 || track >= kTracks) {
        bitStreamValid[track] = true;   // empty; caller wraps mod 0 → 0
        return;
    }
    bits.reserve(static_cast<size_t>(kNibblesPerTrack) * 9);

    const auto& buf = tracks[track];
    const bool noSyncPad = nibFormat;   // .nib has no sync semantics

    // Detect whether nibble[i] is part of a 2+-byte $FF run by checking
    // its neighbours (with wrap-around so the tail stretches into the
    // next-revolution leading bytes correctly).
    auto isInFFRun = [&](int i) {
        if (noSyncPad) return false;
        if (buf[i] != 0xFF) return false;
        const int prev = (i - 1 + kNibblesPerTrack) % kNibblesPerTrack;
        const int next = (i + 1) % kNibblesPerTrack;
        return buf[prev] == 0xFF || buf[next] == 0xFF;
    };

    for (int i = 0; i < kNibblesPerTrack; ++i) {
        const uint8_t b = buf[i];
        // 8 data cells, MSB-first.
        for (int bit = 7; bit >= 0; --bit) {
            bits.push_back(static_cast<uint8_t>((b >> bit) & 1));
        }
        // Sync padding: 2 trailing zero cells for $FF in a run.
        if (isInFFRun(i)) {
            bits.push_back(0);
            bits.push_back(0);
        }
    }
    bitStreamValid[track] = true;
}

int DiskImage::trackBitLength(int track) const
{
    if (track < 0 || track >= kTracks) return 0;
    if (!bitStreamValid[track]) expandTrackBits(track);
    return static_cast<int>(bitStream[track].size());
}

uint8_t DiskImage::bitAt(int track, int bitIdx) const
{
    if (track < 0 || track >= kTracks) return 0;
    if (!bitStreamValid[track]) expandTrackBits(track);
    const auto& bits = bitStream[track];
    if (bits.empty()) return 0;
    const int n = static_cast<int>(bits.size());
    return bits[((bitIdx % n) + n) % n];
}

// ── MAME flux event view ───────────────────────────────────────────────
//
// Verbatim port of `floppy_image_device::cache_fill` data layout, adapted
// to LSS cycles. For each "1" bit cell at index k in the bit-cell stream,
// we emit one flux event at LSS cycle `k * 8 + 4` (cell center). For "0"
// cells we emit nothing — the LSS sees those as the gap between events,
// which is exactly MAME's continuous-flux model: flux changes are point
// events; absence of an event over an 8-LSS-cycle window means a "0".
//
// Cells per byte mirrors `expandTrackBits` (8 cells/byte; sync $FF runs
// pad +2 zero cells per byte). So the total period in LSS cycles equals
// `bitStream.size() * 8`, and PULSE timing under the flux model matches
// PULSE timing the bit-cell view would produce — by construction.
void DiskImage::expandTrackFlux(int track) const
{
    auto& flux = fluxStream[track];
    flux.clear();
    if (track < 0 || track >= kTracks) {
        fluxStreamValid[track] = true;
        return;
    }
    if (!bitStreamValid[track]) expandTrackBits(track);
    const auto& bits = bitStream[track];
    flux.reserve(bits.size() / 2);            // ~half the cells are 1
    for (int i = 0; i < static_cast<int>(bits.size()); ++i) {
        if (bits[i]) flux.push_back(i * 8 + 4);
    }
    fluxStreamValid[track] = true;
}

int DiskImage::trackPeriod(int track) const
{
    return trackBitLength(track) * 8;
}

const std::vector<int>& DiskImage::fluxEvents(int track) const
{
    static const std::vector<int> empty;
    if (track < 0 || track >= kTracks) return empty;
    if (!fluxStreamValid[track]) expandTrackFlux(track);
    return fluxStream[track];
}

// MAME `floppy_image_device::get_next_transition` — returns the time of
// the next flux event at or after `fromLssCycle`. If none in the current
// revolution, wraps and returns the first event of the next revolution
// (offset by one period, so the result remains ≥ fromLssCycle). Only
// returns kFluxNever when the track has no flux events at all (blank
// disk), matching MAME's `attotime::never` for the empty-track case.
int64_t DiskImage::getNextTransition(int track, int64_t fromLssCycle) const
{
    if (track < 0 || track >= kTracks) return kFluxNever;
    const auto& flux = fluxEvents(track);
    if (flux.empty()) return kFluxNever;
    const int period = trackPeriod(track);
    if (period <= 0) return kFluxNever;

    // Reduce fromLssCycle into [0, period) for the lookup; remember the
    // base so we can re-add it to the returned timestamp.
    int64_t base = (fromLssCycle / period) * period;
    int     pos  = static_cast<int>(fromLssCycle - base);
    // upper_bound — first element > pos. We then back up by one to find
    // the largest ≤ pos, but for "next AT OR AFTER" we want the first ≥
    // pos. lower_bound suits us better.
    auto it = std::lower_bound(flux.begin(), flux.end(), pos);
    if (it == flux.end()) {
        // Wrap to next revolution.
        return base + period + flux.front();
    }
    return base + *it;
}

// MAME `floppy_image_device::write_flux` — splice `count` flux events
// (LSS-cycle timestamps in `transitions`) into the track over the LSS-
// cycle range `[startLssCycle, endLssCycle)`. The operation is
// idempotent: any prior flux events in that range are replaced.
//
// POM2 stores tracks as nibbles, not as a flux array, so the splice is
// realised by:
//   1. Computing the cell-index range covered by [start, end) (each cell
//      is 8 LSS cycles wide).
//   2. For each cell window, deciding bit = 1 iff any of the supplied
//      transitions falls strictly inside the cell's interior window
//      [k*8 + 1, k*8 + 7) — matches MAME's PULSE-detect window when the
//      LSS later reads back this region.
//   3. Re-packing 8 consecutive cells per byte into the nibble buffer at
//      the byte index `cell_start_byte = startCell / 8`. Cells past the
//      first byte boundary that don't fit a full byte are deferred until
//      the next splice — which mirrors how the LSS write side flushes in
//      32-event chunks (≥ ~1 byte each).
//
// The bit→nibble re-pack assumes 8 cells per byte (no sync padding) for
// the written region. That matches what the LSS actually writes: it
// shifts 8 bits per byte regardless of WP run. Subsequent reads of the
// re-packed nibble buffer will re-introduce sync padding for any $FF
// nibble that ends up in a 2+ run, which is exactly what real Disk II
// hardware does on read-back.
void DiskImage::writeFlux(int track, int64_t startLssCycle, int64_t endLssCycle,
                          int count, const int64_t* transitions)
{
    if (!loaded || track < 0 || track >= kTracks) return;
    if (endLssCycle <= startLssCycle) return;
    // First-cut WOZ support is read-only. The bit cells in `bitStream`
    // are canonical; running the splice would also re-pack into the
    // (unused) `tracks[]` nibble buffer and invalidate the bit stream
    // (forcing a re-derivation from nibbles that wipes the WOZ data).
    // Software shouldn't attempt writes anyway because isWriteProtected
    // returns true for WOZ images.
    if (wozFormat) return;

    const int period = trackPeriod(track);
    if (period <= 0) return;

    // Reduce both endpoints into [0, period). If the window wraps the
    // revolution boundary, recurse on the two halves.
    int64_t startMod = ((startLssCycle % period) + period) % period;
    int64_t endMod   = startMod + (endLssCycle - startLssCycle);
    if (endMod > period) {
        // Split: [startMod, period) and [0, endMod - period).
        std::vector<int64_t> firstHalf, secondHalf;
        firstHalf.reserve(count);
        secondHalf.reserve(count);
        const int64_t origBase = startLssCycle - startMod;
        const int64_t splitAt  = origBase + period;
        for (int i = 0; i < count; ++i) {
            if (transitions[i] < splitAt) firstHalf.push_back(transitions[i]);
            else                          secondHalf.push_back(transitions[i]);
        }
        writeFlux(track, startLssCycle, splitAt,
                  static_cast<int>(firstHalf.size()), firstHalf.data());
        writeFlux(track, splitAt, endLssCycle,
                  static_cast<int>(secondHalf.size()), secondHalf.data());
        return;
    }

    // Cell-window the [startMod, endMod) range. Inclusive of partial
    // cells at the start and end so a sub-cell-wide write doesn't drop
    // bits.
    const int firstCell = static_cast<int>(startMod / 8);
    const int lastCell  = static_cast<int>((endMod + 7) / 8);   // exclusive

    // Map each transition into a cell index and mark that cell's bit = 1.
    // PULSE-detect window per MAME: address bit 4 goes low for exactly one
    // LSS cycle, at the cycle equal to the transition's timestamp. Any
    // transition timestamped inside [k*8, k*8+8) lights cell k.
    const int64_t origBase = startLssCycle - startMod;
    std::vector<bool> newBits(lastCell - firstCell, false);
    for (int i = 0; i < count; ++i) {
        const int64_t t = ((transitions[i] - origBase) % period + period) % period;
        const int cell  = static_cast<int>(t / 8) - firstCell;
        if (cell >= 0 && cell < static_cast<int>(newBits.size())) {
            newBits[cell] = true;
        }
    }

    // Pack 8 cells per byte starting at the first complete byte boundary
    // inside [firstCell, lastCell). Partial bytes at the leading or
    // trailing edge are dropped — the LSS naturally flushes only complete
    // shifter contents (8 cells = 1 nibble), so writing a half-cell-aligned
    // region during a splice would only happen on a write that didn't
    // complete a full byte, which DOS / RWTS never does.
    auto& buf = tracks[track];
    bool changed = false;
    for (int byteIdx = (firstCell + 7) / 8; byteIdx * 8 + 8 <= firstCell + static_cast<int>(newBits.size()); ++byteIdx) {
        uint8_t v = 0;
        for (int b = 0; b < 8; ++b) {
            const int cellWithinSpan = byteIdx * 8 + b - firstCell;
            if (cellWithinSpan >= 0 && cellWithinSpan < static_cast<int>(newBits.size())
                && newBits[cellWithinSpan]) {
                v |= static_cast<uint8_t>(0x80 >> b);
            }
        }
        const int n = ((byteIdx % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack;
        if (buf[n] != v) {
            buf[n] = v;
            changed = true;
        }
    }
    if (changed) {
        dirty[track]    = true;
        anyDirty        = true;
        invalidateBitStream(track);
    }
}

void DiskImage::nibblizeTrack(int track, const uint8_t* sectors, uint8_t volume,
                              const int* logicalForPhysical)
{
    auto& buf = tracks[track];
    buf.fill(0xFF);              // any unwritten tail stays as sync nibbles
    uint8_t* dst = buf.data();

    for (int physical = 0; physical < kSectorsPerTrack; ++physical) {
        // Sync gap before each sector. 14 × $FF is plenty for the Disk II
        // data separator to lock in (real disks vary 5-40).
        for (int i = 0; i < 14; ++i) *dst++ = 0xFF;

        writeAddressField(dst, volume, static_cast<uint8_t>(track),
                          static_cast<uint8_t>(physical));

        for (int i = 0; i < 5; ++i) *dst++ = 0xFF;

        const int logical = logicalForPhysical[physical];
        writeDataField(dst, sectors + logical * kSectorBytes);
    }
}

void DiskImage::writeAddressField(uint8_t*& dst, uint8_t volume,
                                  uint8_t track, uint8_t sector)
{
    *dst++ = 0xD5; *dst++ = 0xAA; *dst++ = 0x96;
    write4and4(dst, volume);
    write4and4(dst, track);
    write4and4(dst, sector);
    write4and4(dst, static_cast<uint8_t>(volume ^ track ^ sector));
    *dst++ = 0xDE; *dst++ = 0xAA; *dst++ = 0xEB;
}

// ── Decoder ─────────────────────────────────────────────────────────────

bool DiskImage::decodeTrack(int track, uint8_t outSectors[kSectorsPerTrack][kSectorBytes]) const
{
    if (track < 0 || track >= kTracks) return false;
    const auto& buf = tracks[track];
    bool decodedAny = false;

    // Wrap-around scan: walk 2× the track length so a sector that
    // straddles the buffer end can be matched. Index modulo'd into buf.
    auto at = [&](int i) -> uint8_t {
        return buf[((i % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack];
    };

    int curSector = -1;
    for (int i = 0; i < 2 * kNibblesPerTrack; ++i) {
        // Address-field prologue: D5 AA 96.
        if (at(i) == 0xD5 && at(i + 1) == 0xAA && at(i + 2) == 0x96) {
            // 4-and-4 fields: vol, trk, sec, chk (8 nibbles).
            uint8_t addr[4];
            for (int k = 0; k < 4; ++k) {
                uint8_t pair[2] = { at(i + 3 + k * 2), at(i + 4 + k * 2) };
                addr[k] = decode4and4(pair);
            }
            curSector = addr[2];
            // Skip past the address field (3 + 8 + 3 epilogue = 14).
            i += 13;
            continue;
        }
        // Data-field prologue: D5 AA AD.
        if (at(i) == 0xD5 && at(i + 1) == 0xAA && at(i + 2) == 0xAD &&
            curSector >= 0 && curSector < kSectorsPerTrack) {
            // 86 secondary nibbles + 256 primary nibbles + 1 checksum.
            const int dataStart = i + 3;
            uint8_t low2[86];
            uint8_t high6[256];
            uint8_t prev = 0;
            bool ok = true;
            // The encoder's stream order is `for j=85 down to 0: write
            // low2_enc[j]`, where low2_enc[85-i] = src[i]'s packed low2.
            // After decoding disk[0..85] in stream order, decoded[j] =
            // low2_enc[85-j] = src[j]'s packed low2 bits. So our local
            // `low2[j]` (indexed by source byte slot) gets decoded[j].
            for (int j = 0; j < 86; ++j) {
                const uint8_t disk = at(dataStart + j);
                const uint8_t v6   = kGcrInverse[disk];
                if (v6 == 0xFF) { ok = false; break; }
                const uint8_t cur  = static_cast<uint8_t>(prev ^ v6);
                low2[j] = cur;
                prev = cur;
            }
            if (ok) {
                for (int j = 0; j < 256; ++j) {
                    const uint8_t disk = at(dataStart + 86 + j);
                    const uint8_t v6   = kGcrInverse[disk];
                    if (v6 == 0xFF) { ok = false; break; }
                    const uint8_t cur  = static_cast<uint8_t>(prev ^ v6);
                    high6[j] = cur;
                    prev = cur;
                }
            }
            if (ok) {
                // Checksum nibble must XOR to 0 with the running prev.
                const uint8_t chk = kGcrInverse[at(dataStart + 86 + 256)];
                if (chk == 0xFF || (prev ^ chk) != 0) ok = false;
            }
            if (ok) {
                // Recombine: byte[i] = (high6[i] << 2) | low2 bits for slot i.
                for (int b = 0; b < 256; ++b) {
                    const int slot = b % 86;
                    const uint8_t low2pack = low2[slot];
                    uint8_t lo;
                    if (b < 86)        lo = rev2(static_cast<uint8_t>( low2pack       & 3));
                    else if (b < 172)  lo = rev2(static_cast<uint8_t>((low2pack >> 2) & 3));
                    else                lo = rev2(static_cast<uint8_t>((low2pack >> 4) & 3));
                    outSectors[curSector][b] = static_cast<uint8_t>((high6[b] << 2) | lo);
                }
                decodedAny = true;
            }
            // Skip past the data field (3 + 343 + 3 epilogue = 349).
            i += 348;
            curSector = -1;
            continue;
        }
    }
    return decodedAny;
}

bool DiskImage::saveDirty()
{
    if (!loaded || !anyDirty || !writeBackEnabled) {
        return true;   // nothing to save (or save disabled) — no error
    }

    // .nib: just write the raw nibble buffers verbatim.
    if (nibFormat) {
        std::ofstream f(path, std::ios::binary | std::ios::out);
        if (!f) {
            lastError = "Cannot open " + path + " for write";
            return false;
        }
        for (int t = 0; t < kTracks; ++t) {
            f.write(reinterpret_cast<const char*>(tracks[t].data()), kNibblesPerTrack);
        }
        if (!f) { lastError = "Short write on " + path; return false; }
        dirty.fill(false);
        anyDirty = false;
        pom2::log().info("Disk II", "Saved (.nib): " + path);
        return true;
    }

    // .dsk/.do/.po: read existing file, decode dirty tracks, overwrite.
    std::vector<uint8_t> bytes(kBytesPerImage, 0);
    {
        std::ifstream rf(path, std::ios::binary);
        if (rf) rf.read(reinterpret_cast<char*>(bytes.data()), kBytesPerImage);
        // Missing/short read is ok — we'll fill from decode below for
        // every dirty track, leaving non-dirty tracks at 0 (worst case).
    }

    const int* skew = (sectorOrder == SectorOrder::ProDOS)
                      ? kProDosLogicalForPhysical
                      : kDos33LogicalForPhysical;

    int decodedTracks = 0;
    for (int t = 0; t < kTracks; ++t) {
        if (!dirty[t]) continue;
        uint8_t sectors[kSectorsPerTrack][kSectorBytes];
        // Pre-fill with the existing file content so sectors that fail
        // to decode (or weren't rewritten by the guest) keep their
        // original bytes.
        for (int p = 0; p < kSectorsPerTrack; ++p) {
            const int logical = skew[p];
            const size_t off  = (t * kSectorsPerTrack + logical) * kSectorBytes;
            std::memcpy(sectors[p], bytes.data() + off, kSectorBytes);
        }
        if (!decodeTrack(t, sectors)) continue;   // no parseable sector
        // Re-pack into the file at logical positions.
        for (int p = 0; p < kSectorsPerTrack; ++p) {
            const int logical = skew[p];
            const size_t off  = (t * kSectorsPerTrack + logical) * kSectorBytes;
            std::memcpy(bytes.data() + off, sectors[p], kSectorBytes);
        }
        ++decodedTracks;
    }

    std::ofstream wf(path, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!wf) { lastError = "Cannot open " + path + " for write"; return false; }
    wf.write(reinterpret_cast<const char*>(bytes.data()), kBytesPerImage);
    if (!wf) { lastError = "Short write on " + path; return false; }
    dirty.fill(false);
    anyDirty = false;
    pom2::log().info("Disk II", "Saved " + std::to_string(decodedTracks) +
                     " modified track(s) to " + path);
    return true;
}

// ────────────────────────────────────────────────────────────────────────

void DiskImage::writeDataField(uint8_t*& dst, const uint8_t* src)
{
    *dst++ = 0xD5; *dst++ = 0xAA; *dst++ = 0xAD;

    // Split the 256 input bytes into the 6-and-2 buffers. high6 holds
    // the top 6 bits in normal order; low2[i] packs the low 2 bits of
    // src[i], src[i+86], src[i+172] into one nibble (bit-pair-swapped).
    uint8_t high6[256];
    uint8_t low2[86];
    for (int i = 0; i < 256; ++i) high6[i] = static_cast<uint8_t>(src[i] >> 2);
    // Index reversal: the boot PROM's $0300+X buffer reads in the order
    // disk-nibble 0 → $0300+85, disk-nibble 1 → $0300+84, … so on the
    // combine pass it pulls byte[i]'s low-2 bits from low2[85 - i mod 86]
    // (slot i / 86), NOT low2[i mod 86]. Mirror that here so the on-disk
    // layout matches what the controller's RWTS reconstructs.
    for (int i = 0; i < 86; ++i) {
        uint8_t v = rev2(src[i] & 3);
        if (i + 86  < 256) v |= static_cast<uint8_t>(rev2(src[i + 86]  & 3) << 2);
        if (i + 172 < 256) v |= static_cast<uint8_t>(rev2(src[i + 172] & 3) << 4);
        low2[85 - i] = v;
    }

    // Stream out: low2 in REVERSE order (85→0), then high6 normally
    // (0→255), running an XOR checksum the whole way. Each nibble is
    // translated through kGcrTable on its way to the disk surface.
    uint8_t prev = 0;
    for (int j = 85; j >= 0; --j) {
        uint8_t cur = low2[j];
        *dst++ = kGcrTable[(prev ^ cur) & 0x3F];
        prev = cur;
    }
    for (int i = 0; i < 256; ++i) {
        uint8_t cur = high6[i];
        *dst++ = kGcrTable[(prev ^ cur) & 0x3F];
        prev = cur;
    }
    *dst++ = kGcrTable[prev & 0x3F];   // final XOR checksum nibble

    *dst++ = 0xDE; *dst++ = 0xAA; *dst++ = 0xEB;
}

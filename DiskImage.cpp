// POM2 Apple II Emulator
// Copyright (C) 2026

#include "DiskImage.h"
#include "Logger.h"

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
    //   .nib → raw nibble stream (no encoding)
    //   .po  → ProDOS sector order
    //   else → DOS 3.3 sector order (.dsk, .do, or no extension)
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
        sectorOrder = SectorOrder::Dos33;     // not meaningful for .nib
        dirty.fill(false);
        anyDirty    = false;
        lastError.clear();
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
    sectorOrder = order;
    dirty.fill(false);
    anyDirty    = false;
    lastError.clear();
    pom2::log().info("Disk II", "Loaded " + imgPath +
                     " (35 tracks, 16 sectors, GCR-encoded, " +
                     (order == SectorOrder::ProDOS ? "ProDOS" : "DOS 3.3") +
                     " order)");
    return true;
}

void DiskImage::eject()
{
    loaded = false;
    nibFormat = false;
    path.clear();
    for (auto& t : tracks) t.fill(0xFF);
    dirty.fill(false);
    anyDirty = false;
}

void DiskImage::writeNibbleAt(int track, int index, uint8_t value)
{
    if (!loaded || track < 0 || track >= kTracks) return;
    const int n = ((index % kNibblesPerTrack) + kNibblesPerTrack) % kNibblesPerTrack;
    if (tracks[track][n] != value) {
        tracks[track][n] = value;
        dirty[track]     = true;
        anyDirty         = true;
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

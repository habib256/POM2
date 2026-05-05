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
                    "-byte .dsk image (DOS 3.3 order), got " +
                    std::to_string(size);
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
    for (int t = 0; t < kTracks; ++t) {
        nibblizeTrack(t,
            buf.data() + t * kSectorsPerTrack * kSectorBytes,
            kVolume);
    }

    path      = imgPath;
    loaded    = true;
    lastError.clear();
    pom2::log().info("Disk II", "Loaded " + imgPath +
                     " (35 tracks, 16 sectors, GCR-encoded)");
    return true;
}

void DiskImage::eject()
{
    loaded = false;
    path.clear();
    for (auto& t : tracks) t.fill(0xFF);
}

void DiskImage::nibblizeTrack(int track, const uint8_t* sectors, uint8_t volume)
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

        const int logical = kDos33LogicalForPhysical[physical];
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

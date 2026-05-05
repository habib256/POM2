// DiskImage GCR round-trip smoke test.
//
// Pins the 4-and-4 address field encoding and the 6-and-2 data field
// encoding by walking DiskImage's pre-nibblized track buffer with the same
// rules a Disk II controller uses on read-back. If this test passes, our
// .dsk image is structurally sound and any boot failure is in the
// controller / dispatch / timing — not in the encoding itself.
//
// Method: build a 143 360-byte image where every sector i (0..15) of
// track t (0..34) holds a deterministic pattern (so we can detect any
// sector mis-skewing), nibblize it via DiskImage, then for track 0 hunt
// for the address-field prologue and recover all 16 logical sectors
// through the standard decode path.

#include "DiskImage.h"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// 64-byte inverse of DiskImage's GCR write-translate table. Real disk
// nibbles are 0x96..0xFF (sparse); other values are invalid and map to
// 0xFF here as a sentinel for "not a valid GCR byte".
std::array<uint8_t, 256> buildGcrInverse()
{
    static constexpr uint8_t kGcrTable[64] = {
        0x96, 0x97, 0x9A, 0x9B, 0x9D, 0x9E, 0x9F, 0xA6,
        0xA7, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB2, 0xB3,
        0xB4, 0xB5, 0xB6, 0xB7, 0xB9, 0xBA, 0xBB, 0xBC,
        0xBD, 0xBE, 0xBF, 0xCB, 0xCD, 0xCE, 0xCF, 0xD3,
        0xD6, 0xD7, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE,
        0xDF, 0xE5, 0xE6, 0xE7, 0xE9, 0xEA, 0xEB, 0xEC,
        0xED, 0xEE, 0xEF, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
        0xF7, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
    };
    std::array<uint8_t, 256> inv;
    inv.fill(0xFF);
    for (uint8_t i = 0; i < 64; ++i) inv[kGcrTable[i]] = i;
    return inv;
}

// 4-and-4 decode: combine the "odd" and "even" nibble halves back into
// a single byte. ((odd << 1) | 1) & even.
inline uint8_t decode4and4(uint8_t hi, uint8_t lo)
{
    return static_cast<uint8_t>(((hi << 1) | 1) & lo);
}

// Reverse of DiskImage::rev2 — bit 0 ↔ bit 1.
inline uint8_t rev2(uint8_t b) { return ((b & 1) << 1) | ((b >> 1) & 1); }

// Hunt for `pattern` (3 bytes) in `buf`, starting at `pos`. Returns the
// index just past the matched pattern, or `std::string::npos` if none.
size_t findMarker(const uint8_t* buf, size_t len,
                  size_t pos,
                  uint8_t a, uint8_t b, uint8_t c)
{
    // Linear search; the buffer is 6656 bytes so brute force is fine.
    for (size_t i = pos; i + 2 < len; ++i) {
        if (buf[i] == a && buf[i + 1] == b && buf[i + 2] == c) {
            return i + 3;
        }
    }
    return static_cast<size_t>(-1);
}

// Decode one address-field worth (8 nibbles starting at `pos`). On
// success writes vol/track/sector and returns true; checksum mismatch
// returns false.
bool decodeAddressField(const uint8_t* buf, size_t pos,
                        uint8_t& volume, uint8_t& track, uint8_t& sector)
{
    volume = decode4and4(buf[pos + 0], buf[pos + 1]);
    track  = decode4and4(buf[pos + 2], buf[pos + 3]);
    sector = decode4and4(buf[pos + 4], buf[pos + 5]);
    const uint8_t chk = decode4and4(buf[pos + 6], buf[pos + 7]);
    return (volume ^ track ^ sector ^ chk) == 0;
}

// Decode one data field (343 nibbles + 3-byte epilogue not consumed).
// Returns true on checksum success; fills `out` with the recovered 256
// bytes.
bool decodeDataField(const uint8_t* buf, size_t pos,
                     const std::array<uint8_t, 256>& gcrInv,
                     uint8_t* out)
{
    uint8_t low2[86];
    uint8_t high6[256];

    uint8_t prev = 0;
    // 86 secondary nibbles, written reversed (j=85..0).
    for (int j = 85; j >= 0; --j) {
        const uint8_t raw = buf[pos++];
        const uint8_t six = gcrInv[raw];
        if (six == 0xFF) return false;
        prev ^= six;
        low2[j] = prev;
    }
    // 256 primary nibbles in normal order.
    for (int i = 0; i < 256; ++i) {
        const uint8_t raw = buf[pos++];
        const uint8_t six = gcrInv[raw];
        if (six == 0xFF) return false;
        prev ^= six;
        high6[i] = prev;
    }
    // Final XOR-checksum nibble.
    const uint8_t rawChk = buf[pos++];
    const uint8_t sixChk = gcrInv[rawChk];
    if (sixChk == 0xFF) return false;
    if ((prev ^ sixChk) != 0) return false;     // checksum mismatch

    // Reconstruct 256 bytes — mirror the boot PROM's combine pass exactly.
    // The PROM pulls byte[i]'s low-2 bits from low2[85 - (i mod 86)] at
    // slot (i / 86), then swaps the bit-pair (LSR-ROL-LSR-ROL undoes the
    // encoder's rev2). Decoding via low2[i mod 86] would mask a sector-
    // skew bug because BOTH encoder and decoder would share the same off-
    // by-86 — keep this index pinned to what the PROM actually does.
    for (int i = 0; i < 256; ++i) {
        const int slot = i / 86;
        const uint8_t pair = (low2[85 - (i % 86)] >> (slot * 2)) & 0x3;
        out[i] = static_cast<uint8_t>((high6[i] << 2) | rev2(pair));
    }
    return true;
}

void buildSyntheticImage(std::vector<uint8_t>& buf)
{
    buf.assign(DiskImage::kBytesPerImage, 0);
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        for (int s = 0; s < DiskImage::kSectorsPerTrack; ++s) {
            const size_t base = (t * DiskImage::kSectorsPerTrack + s)
                                * DiskImage::kSectorBytes;
            // Pattern = top 8 bits of sector index OR'd with byte index,
            // so a misordered sector or misaligned byte trips the assert.
            const uint8_t tag = static_cast<uint8_t>((t << 4) | s);
            for (int b = 0; b < DiskImage::kSectorBytes; ++b) {
                buf[base + b] = static_cast<uint8_t>(tag ^ b);
            }
        }
    }
}

std::string writeTempImage(const std::vector<uint8_t>& buf)
{
    // Use a fixed name in /tmp (or current dir) — ctest runs each test in
    // its own working dir, so collisions across tests are unlikely.
    const std::string path = "disk_image_smoke.dsk";
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    assert(f && "cannot create temp .dsk");
    f.write(reinterpret_cast<const char*>(buf.data()),
            static_cast<std::streamsize>(buf.size()));
    f.close();
    return path;
}

void readNibbleBuffer(const DiskImage& img, int track,
                      std::vector<uint8_t>& out)
{
    out.resize(DiskImage::kNibblesPerTrack);
    for (int i = 0; i < DiskImage::kNibblesPerTrack; ++i) {
        out[i] = img.nibbleAt(track, i);
    }
}

}  // namespace

int main()
{
    const auto gcrInv = buildGcrInverse();

    // Build + load image.
    std::vector<uint8_t> source;
    buildSyntheticImage(source);
    const std::string path = writeTempImage(source);

    DiskImage img;
    if (!img.loadFile(path)) {
        std::fprintf(stderr, "loadFile failed: %s\n",
                     img.getLastError().c_str());
        std::remove(path.c_str());
        return 1;
    }

    // Walk track 0's nibble buffer and recover all 16 sectors. We expect
    // to find D5 AA 96 sixteen times; for each, decode the address field
    // (vol must be 254, track 0, sector in 0..15 unique), then find the
    // following D5 AA AD and decode the data.
    std::vector<uint8_t> nibs;
    readNibbleBuffer(img, 0, nibs);

    std::array<bool, 16> sawSector{};
    size_t pos = 0;
    int found = 0;
    while (true) {
        const size_t ap = findMarker(nibs.data(), nibs.size(), pos,
                                     0xD5, 0xAA, 0x96);
        if (ap == static_cast<size_t>(-1)) break;

        uint8_t vol, trk, sec;
        if (!decodeAddressField(nibs.data(), ap, vol, trk, sec)) {
            std::fprintf(stderr, "address field checksum failed at %zu\n", ap);
            std::remove(path.c_str());
            return 2;
        }
        if (vol != 254) {
            std::fprintf(stderr, "vol mismatch: got %u want 254\n", vol);
            std::remove(path.c_str());
            return 3;
        }
        if (trk != 0) {
            std::fprintf(stderr, "track mismatch: got %u want 0\n", trk);
            std::remove(path.c_str());
            return 4;
        }
        if (sec >= 16 || sawSector[sec]) {
            std::fprintf(stderr, "sector %u out of range or duplicated\n", sec);
            std::remove(path.c_str());
            return 5;
        }
        sawSector[sec] = true;

        // Find the data field that goes with this address field.
        const size_t dp = findMarker(nibs.data(), nibs.size(), ap + 8,
                                     0xD5, 0xAA, 0xAD);
        if (dp == static_cast<size_t>(-1)) {
            std::fprintf(stderr, "no data field after addr sector %u\n", sec);
            std::remove(path.c_str());
            return 6;
        }
        uint8_t recovered[256];
        if (!decodeDataField(nibs.data(), dp, gcrInv, recovered)) {
            std::fprintf(stderr,
                         "data field checksum failed for sector %u\n", sec);
            std::remove(path.c_str());
            return 7;
        }
        // The address field's sector number is the PHYSICAL index
        // (0..15 around the disk). The data inside this physical slot
        // belongs to the LOGICAL sector that DOS 3.3's skew table maps
        // to, mirroring DiskImage.cpp's encoder.
        static constexpr int kDos33LogicalForPhysical[16] = {
            0, 7, 14, 6, 13, 5, 12, 4, 11, 3, 10, 2, 9, 1, 8, 15
        };
        const int logical = kDos33LogicalForPhysical[sec];
        const uint8_t* expected = source.data() + logical * 256;
        if (std::memcmp(recovered, expected, 256) != 0) {
            std::fprintf(stderr,
                "physical %u (logical %d) data mismatch;"
                " first byte got 0x%02X want 0x%02X\n",
                sec, logical, recovered[0], expected[0]);
            std::remove(path.c_str());
            return 8;
        }
        ++found;
        pos = dp + 343;     // skip past the data field we just consumed
    }

    std::remove(path.c_str());

    if (found != 16) {
        std::fprintf(stderr, "found %d address fields on track 0, want 16\n",
                     found);
        return 9;
    }
    for (int i = 0; i < 16; ++i) {
        if (!sawSector[i]) {
            std::fprintf(stderr, "missing logical sector %d on track 0\n", i);
            return 10;
        }
    }

    std::printf("disk_image_smoke OK: track 0 round-trips all 16 sectors\n");
    return 0;
}

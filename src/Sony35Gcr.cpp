// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Sony35Gcr — see the header. Moved here verbatim from `Sony35Drive.cpp`
// (2026-08-18) so the WOZ loader in `Disk35Image` reads flux with the same
// tables and the same checksum walk the drive does, rather than a second
// transcription of MAME's decoder.

#include "Sony35Gcr.h"

#include <cstring>

namespace pom2 {
namespace sony35 {
namespace {

// the Disk II / IWM data separator needs).
//
// `kGcr6bw[]` is the read-side inverse (MAME `flopimg.cpp:979-997`).
// Indexed by an 8-bit disk byte; returns the 6-bit value, or 0 for
// invalid encodings.
constexpr uint8_t kGcr6bw[0x100] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x02, 0x03, 0x00, 0x04, 0x05, 0x06,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x08, 0x00, 0x00, 0x00, 0x09, 0x0a, 0x0b, 0x0c, 0x0d,
    0x00, 0x00, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x00, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1b, 0x00, 0x1c, 0x1d, 0x1e,
    0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x20, 0x21, 0x00, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x29, 0x2a, 0x2b, 0x00, 0x2c, 0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32,
    0x00, 0x00, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x00, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e, 0x3f,
};

// MAME `flopimg.cpp:522-533 gcr6_decode` — 4 disk bytes → 3 raw bytes.
inline void gcr6Decode(uint8_t e0, uint8_t e1, uint8_t e2, uint8_t e3,
                       uint8_t& va, uint8_t& vb, uint8_t& vc)
{
    e0 = kGcr6bw[e0];
    e1 = kGcr6bw[e1];
    e2 = kGcr6bw[e2];
    e3 = kGcr6bw[e3];
    va = static_cast<uint8_t>(((e0 << 2) & 0xc0) | e1);
    vb = static_cast<uint8_t>(((e0 << 4) & 0xc0) | e2);
    vc = static_cast<uint8_t>(((e0 << 6) & 0xc0) | e3);
}

}  // namespace

int blockIndexFor(int track, int head, int sector)
{
    int idx = 0;
    for (int t = 0; t < track; ++t) idx += 2 * sectorsForTrack(t);
    idx += head * sectorsForTrack(track);
    return idx + sector;
}

//   3. Terminate when `pos < 8` after the skip — meaning we've
//      wrapped past cell 0, so the byte we just emitted closes the
//      revolution.
std::vector<uint8_t> nibblesFromCells(const std::vector<uint8_t>& cells)
{
    std::vector<uint8_t> out;
    const int n = static_cast<int>(cells.size());
    if (n < 8) return out;

    // ─── Initial alignment (MAME line 1535-1551) ──────────────────────
    int pos = 0;
    while (pos < n) {
        while (pos < n && !cells[pos]) ++pos;
        if (pos == n) {
            pos = 0;
            while (pos < n && !cells[pos]) ++pos;
            if (pos == n) return out;                  // unformatted track
            goto found;
        }
        pos += 8;
    }
    while (pos >= n) pos -= n;
    while (pos < n && !cells[pos]) ++pos;
    if (pos == n) return out;   // alignment wrap landed in an all-zero tail;
                                // without this, the found: reader dereferences
                                // cells[n] (1-byte heap over-read). Mirrors the
                                // guard on the goto-found path above.
 found:

    out.reserve(static_cast<size_t>(n) / 8 + 8);
    for (;;) {
        uint8_t v = 0;
        for (int i = 0; i < 8; ++i) {
            if (cells[pos]) v |= static_cast<uint8_t>(0x80 >> i);
            ++pos;
            if (pos == n) pos = 0;
        }
        out.push_back(v);
        if (pos < 8) return out;                       // wrapped past cell 0
        while (pos < n && !cells[pos]) ++pos;
        if (pos == n) return out;
    }
}

std::vector<uint8_t> cellsFromPackedBits(const uint8_t* data, std::size_t len,
                                         uint32_t bitCount)
{
    std::vector<uint8_t> cells;
    if (!data || !len || !bitCount) return cells;
    const uint32_t n = (bitCount < len * 8u) ? bitCount
                                             : static_cast<uint32_t>(len * 8u);
    cells.resize(n);
    for (uint32_t i = 0; i < n; ++i)
        cells[i] = static_cast<uint8_t>((data[i >> 3] >> (7 - (i & 7))) & 1);
    return cells;
}

int decodeSectors(const std::vector<uint8_t>& nib, int expectTrack,
                  const SectorSink& sink)
{
    if (nib.size() < 300 || !sink) return 0;

    // Find every D5AA96 address-prologue position (MAME line 2133-2138).
    std::vector<int> hpos;
    uint32_t hstate = (static_cast<uint32_t>(nib[nib.size() - 2]) << 8) |
                      nib[nib.size() - 1];
    for (int p = 0; p < static_cast<int>(nib.size()); ++p) {
        hstate = ((hstate << 8) | nib[p]) & 0xFFFFFF;
        if (hstate == 0xD5AA96) {
            hpos.push_back(p == static_cast<int>(nib.size()) - 1 ? 0 : p + 1);
        }
    }

    int written = 0;
    const int nibSz = static_cast<int>(nib.size());
    auto wrap = [nibSz](int p) { return p % nibSz; };

    for (int startPos : hpos) {
        int pos = startPos;
        uint8_t h[7];
        for (int i = 0; i < 7; ++i) {
            h[i] = nib[wrap(pos)];
            pos = wrap(pos + 1);
        }

        // Address-field decode (MAME line 2152-2161).
        const uint8_t v2 = kGcr6bw[h[2]];
        const uint8_t v3 = kGcr6bw[h[3]];
        const uint8_t tr = static_cast<uint8_t>(
            kGcr6bw[h[0]] | ((v2 & 1) ? 0x40 : 0x00));
        const uint8_t se  = kGcr6bw[h[1]];
        const uint8_t c1  = (tr ^ se ^ v2 ^ v3) & 0x3f;
        const uint8_t chk = kGcr6bw[h[4]];
        const int head = (v2 & 0x20) ? 1 : 0;
        if (chk != c1 || tr >= 80 || se >= sectorsForTrack(tr) ||
            h[5] != 0xDE || h[6] != 0xAA) continue;
        if (expectTrack >= 0 && static_cast<int>(tr) != expectTrack) continue;

        // Scan ahead for the matching D5AAAD data prologue (line 2165-2179).
        uint32_t st = (static_cast<uint32_t>(nib[wrap(pos)]) << 8);
        pos = wrap(pos + 1);
        st |= nib[wrap(pos)];
        pos = wrap(pos + 1);
        bool foundData = false;
        for (int guard = 0; guard < nibSz; ++guard) {
            st = ((st << 8) | nib[wrap(pos)]) & 0xFFFFFF;
            pos = wrap(pos + 1);
            if (st == 0xD5AA96) break;             // ran into next sector
            if (st == 0xD5AAAD) { foundData = true; break; }
        }
        if (!foundData) continue;

        // Skip the sector-number duplicate byte (MAME line 2182).
        pos = wrap(pos + 1);

        // Decode 175 groups of 4-in/3-out (line 2187-2210).
        uint8_t sdata[524];
        std::memset(sdata, 0, sizeof(sdata));
        uint8_t ca = 0, cb = 0, cc = 0;
        // No early-out here on purpose: `gcr6Decode` maps every one of the
        // 256 possible nibbles through `kGcr6bw`, so there is no "invalid
        // byte" for it to report — a corrupt group decodes to garbage and is
        // caught below by the running checksum plus the DE AA epilogue.
        // (This loop used to carry a `decodeOk` flag that nothing ever
        // cleared, which read as if bad GCR were rejected here. It wasn't.)
        for (int i = 0; i < 175; ++i) {
            uint8_t e0 = nib[wrap(pos)]; pos = wrap(pos + 1);
            uint8_t e1 = nib[wrap(pos)]; pos = wrap(pos + 1);
            uint8_t e2 = nib[wrap(pos)]; pos = wrap(pos + 1);
            uint8_t e3 = (i < 174) ? nib[wrap(pos)] : 0x96;
            if (i < 174) pos = wrap(pos + 1);
            uint8_t va, vb, vc;
            gcr6Decode(e0, e1, e2, e3, va, vb, vc);
            cc = static_cast<uint8_t>((cc << 1) | (cc >> 7));
            va = static_cast<uint8_t>(va ^ cc);
            const uint16_t suma = static_cast<uint16_t>(ca + va + (cc & 1));
            ca = static_cast<uint8_t>(suma);
            vb = static_cast<uint8_t>(vb ^ ca);
            const uint16_t sumb = static_cast<uint16_t>(cb + vb + (suma >> 8));
            cb = static_cast<uint8_t>(sumb);
            vc = static_cast<uint8_t>(vc ^ cb);
            sdata[3 * i + 0] = va;
            sdata[3 * i + 1] = vb;
            if (i != 174) {
                cc = static_cast<uint8_t>(cc + vc + (sumb >> 8));
                sdata[3 * i + 2] = vc;
            }
        }
        // Data-field checksum + DE AA epilogue (line 2213-2220).
        uint8_t epi[6];
        for (int i = 0; i < 6; ++i) {
            epi[i] = nib[wrap(pos)];
            pos = wrap(pos + 1);
        }
        uint8_t va, vb, vc;
        gcr6Decode(epi[0], epi[1], epi[2], epi[3], va, vb, vc);
        if (va != ca || vb != cb || vc != cc ||
            epi[4] != 0xDE || epi[5] != 0xAA) {
            continue;
        }

        // sdata[0..11] are tag bytes (zero in .po files), sdata[12..523]
        // is the 512-byte ProDOS block payload.
        sink(tr, head, se, sdata + 12);
        ++written;
    }
    return written;
}

}  // namespace sony35
}  // namespace pom2

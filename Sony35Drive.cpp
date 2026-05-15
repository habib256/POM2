// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Port of MAME's Apple 3.5" Sony drive model (`applefdintf_device::add_35`
// + `floppy_image_device` SENSE / phase decoder). The "phases-as-command"
// protocol is documented in *Inside the Apple //gs* hardware reference
// chapter 6 ("Sony Drive"); MAME's authoritative source is in
// `src/devices/imagedev/mac_floppy.cpp::seek_phase_w` and
// `::wpt_r`. Where MAME would consult the loaded `floppy_image` for
// per-track state (write-protect tab, disk-in-drive switch, etc.), POM2
// queries the attached `Disk35Image`.
//
// Register table (read & write both indexed by { SEL, CA2, CA1, CA0 }):
//
//   addr  bits SEL CA2 CA1 CA0   read SENSE        write strobe (LSTRB pulse)
//   ----  ------------------------------------------------------------
//   0x0    0   0   0   0         /DIRTN            (direction set inward)
//   0x1    0   0   0   1         /STEP-in-progress (single step)
//   0x2    0   0   1   0         /MOTOR-ON         motor on
//   0x3    0   0   1   1         /TRACK0           motor off
//   0x4    0   1   0   0         /SWITCHED         (eject — Sony only)
//   0x5    0   1   0   1         (reserved)        (reserved)
//   0x6    0   1   1   0         /TACH             (reserved)
//   0x7    0   1   1   1         (reserved)        (reserved)
//   0x8    1   0   0   0         /SIDES            (set head 0)
//   0x9    1   0   0   1         (reserved)        (set head 1)
//   0xA    1   0   1   0         /READY            (reserved)
//   0xB    1   0   1   1         /INSERTED         (reserved)
//   0xC    1   1   0   0         (reserved)        (reserved)
//   0xD    1   1   0   1         /SEL              (reserved)
//   0xE    1   1   1   0         (reserved)        (reserved)
//   0xF    1   1   1   1         /DRVIN            (reserved)
//
// Apple's protocol uses ACTIVE-LOW logic on most read lines (the "/"
// prefix), so a "1" returned to the IWM means the named condition is
// NOT present. We expose this via `senseR()` returning the *raw* bit
// (true = HIGH = inactive).

#include "Sony35Drive.h"
#include "CpuClock.h"
#include "Disk35Image.h"
#include "Logger.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <string>

namespace pom2 {

namespace {

constexpr uint8_t kBitCA0   = 0x01;
constexpr uint8_t kBitCA1   = 0x02;
constexpr uint8_t kBitCA2   = 0x04;
constexpr uint8_t kBitLSTRB = 0x08;

// ── Sony zoned-recording geometry (MAME flopimg.cpp:2019-2027) ──────────
//
// Five concentric speed zones cover the 80 tracks. Each zone keeps the
// IWM bit-cell rate constant (~505 kHz) by spinning the platter slower
// at the outer zones. Cells per revolution × RPM × 60 / 1 µs ≈ constant.
//
// kCellsPerRev[zone] = 30318342 / RPM  (MAME's constant, ticks per cell)
// kRpm[zone]         = nominal spindle RPM at this zone
constexpr int kCellsPerRev[5] = { 76950, 70695, 64234, 57749, 51388 };
constexpr int kRpm[5]         = {   394,   429,   472,   525,   590 };

// Per-zone CPU cycles per revolution. (60 / RPM) seconds × CPU clock.
// Pre-computed because the integer division is sensitive to ordering.
constexpr int64_t kCyclesPerRev[5] = {
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[0],   // 155 745
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[1],   // 143 038
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[2],   // 130 008
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[3],   // 116 883
    static_cast<int64_t>(POM2_CPU_CLOCK_HZ) * 60 / kRpm[4],   // 103 999
};

constexpr int sectorsForTrack35(int track) {
    return 12 - (track / 16);
}
constexpr int zoneForTrack(int track) {
    return std::min(track / 16, 4);
}

// MAME `flopimg.cpp:967-977 gcr6fw_tb` — 64-entry write-side GCR table.
// Maps 6-bit values to the 8-bit "disk bytes" the IWM writes (always
// has bit 7 set, no two adjacent zero bits — the recovery constraints
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

constexpr uint8_t kGcr6fw[0x40] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

// MAME `flopimg.cpp:512-520 gcr6_encode` — 3 raw bytes → 4 GCR bytes.
inline uint32_t gcr6Encode(uint8_t va, uint8_t vb, uint8_t vc)
{
    uint32_t r =
        (static_cast<uint32_t>(kGcr6fw[((va >> 2) & 0x30) |
                                       ((vb >> 4) & 0x0c) |
                                       ((vc >> 6) & 0x03)]) << 24) |
        (static_cast<uint32_t>(kGcr6fw[va & 0x3f]) << 16) |
        (static_cast<uint32_t>(kGcr6fw[vb & 0x3f]) <<  8) |
        (static_cast<uint32_t>(kGcr6fw[vc & 0x3f]));
    return r;
}

// MAME `flopimg.cpp:326 raw_w` — append `n` bit cells (MSB first) from
// `value` to the cell stream. For GCR every cell is the same period
// (the `size` parameter MAME passes is irrelevant on constant-rate
// zones), so we ignore it and just push 0/1 bits.
inline void rawW(std::vector<uint8_t>& cells, int n, uint32_t value)
{
    for (int i = n - 1; i >= 0; --i) {
        cells.push_back(static_cast<uint8_t>((value >> i) & 1));
    }
}

// Block index inside the `.po` payload for (track, head, logicalSec).
// MAME `apple_gcr_format::load` (ap_dsk35.cpp:366-386): blocks are
// laid out linearly by track, then by head, then by logical sector.
// On 800K the count per (track,head) is 12 - (track/16) blocks.
int blockIndexFor(int track, int head, int logicalSec)
{
    int idx = 0;
    for (int t = 0; t < track; ++t) {
        idx += 2 * sectorsForTrack35(t);
    }
    idx += head * sectorsForTrack35(track);
    return idx + logicalSec;
}

// Build the bit-cell stream for one (track, head) pair using the
// Sony 800K GCR layout. Verbatim port of MAME
// `flopimg.cpp:2017-2106 build_mac_track_gcr`. The output is one bit
// per cell, total = kCellsPerRev[zone].
void buildTrackBits(const Disk35Image& img,
                    int track, int head,
                    std::vector<uint8_t>& cells)
{
    const int zone     = zoneForTrack(track);
    const int sectors  = sectorsForTrack35(track);
    const int cellsRev = kCellsPerRev[zone];
    // Per MAME line 2034: pregap = cells_per_speed_zone - 6208 × sectors.
    const int pregap   = cellsRev - 6208 * sectors;

    cells.clear();
    cells.reserve(cellsRev);

    // Prepregap / pregap self-sync (MAME lines 2038-2049). The pattern
    // 0xff3fcf / 0xf3fcff is the two halves of the standard "10 cells
    // per $FF" self-sync run packed into 24-bit chunks.
    const int prepregap = pregap % 48;
    if (prepregap >= 24) {
        rawW(cells, prepregap - 24, 0xff3fcf);
        rawW(cells, 24,             0xf3fcff);
    } else {
        rawW(cells, prepregap,      0xf3fcff);
    }
    for (int i = 0; i < pregap / 48; ++i) {
        rawW(cells, 24, 0xff3fcf);
        rawW(cells, 24, 0xf3fcff);
    }

    // Per-sector loop. Sectors are laid out in physical order 0..ns-1
    // on disk; the address-field "sector number" carries the LOGICAL
    // sector that lives at this physical slot (MAME's 2:1 interleave
    // schedule, `apple_gcr_format::load` lines 372-382).
    auto physicalToLogical = [sectors](int phys) -> int {
        // Invert `si = (si + 2) % ns; if (si == 0) si++;` starting si=0.
        int si = 0;
        for (int logical = 0; logical < sectors; ++logical) {
            if (si == phys) return logical;
            si = (si + 2) % sectors;
            if (si == 0) si++;
        }
        return 0;  // unreachable on valid input
    };

    // Read all `sectors` blocks for this (track, head) into a flat
    // buffer up front — the encoder walks them in physical order so
    // grabbing them once avoids repeated random reads on the image.
    std::array<uint8_t, 12 * 512> sectorData{};
    for (int phys = 0; phys < sectors; ++phys) {
        const int logical = physicalToLogical(phys);
        const int blkIdx  = blockIndexFor(track, head, logical);
        img.readBlock(static_cast<uint32_t>(blkIdx),
                      sectorData.data() + phys * 512);
    }

    constexpr uint8_t kSonyFormatByte = 0x22;   // double-sided Mac/A2 800K
    for (int s = 0; s < sectors; ++s) {
        const int logicalSec = physicalToLogical(s);

        // 8× 48-cell self-sync gap before each sector (MAME 2052-2055).
        for (int i = 0; i < 8; ++i) {
            rawW(cells, 24, 0xff3fcf);
            rawW(cells, 24, 0xf3fcff);
        }

        // Address field (MAME 2057-2066).
        rawW(cells, 24, 0xd5aa96);
        const uint8_t tr  = static_cast<uint8_t>(track);
        const uint8_t sec = static_cast<uint8_t>(logicalSec);
        const uint8_t sid = static_cast<uint8_t>(
            ((tr & 0x40) ? 1 : 0) | (head ? 0x20 : 0));
        const uint8_t fmt = kSonyFormatByte;
        const uint8_t chk =
            static_cast<uint8_t>(tr ^ sec ^ sid ^ fmt);
        rawW(cells, 8, kGcr6fw[tr  & 0x3f]);
        rawW(cells, 8, kGcr6fw[sec & 0x3f]);
        rawW(cells, 8, kGcr6fw[sid & 0x3f]);
        rawW(cells, 8, kGcr6fw[fmt & 0x3f]);
        rawW(cells, 8, kGcr6fw[chk & 0x3f]);
        rawW(cells, 24, 0xdeaaff);

        // Inter-field self-sync (MAME 2068-2069).
        rawW(cells, 24, 0xff3fcf);
        rawW(cells, 24, 0xf3fcff);

        // Data field (MAME 2071-2103).
        rawW(cells, 24, 0xd5aaad);
        rawW(cells, 8,  kGcr6fw[sec & 0x3f]);

        const uint8_t* secBytes = sectorData.data() + s * 512;
        // MAME pre-pends 12 tag bytes (zero on Apple GCR loads — there
        // is no tag region in the .po format). Iterate 175 nibble
        // groups of 3 in 4-out; the first 4 groups consume the (zero)
        // tag bytes, the rest the 512-byte data payload.
        std::array<uint8_t, 12 + 512 + 1> dataWithTag{};
        std::memcpy(dataWithTag.data() + 12, secBytes, 512);
        // dataWithTag[524] is the implicit `vc = 0` for i==174.

        uint8_t ca = 0, cb = 0, cc = 0;
        for (int i = 0; i < 175; ++i) {
            const uint8_t va = dataWithTag[3 * i + 0];
            const uint8_t vb = dataWithTag[3 * i + 1];
            const uint8_t vc = (i != 174) ? dataWithTag[3 * i + 2] : 0;

            cc = static_cast<uint8_t>((cc << 1) | (cc >> 7));
            const uint16_t suma = static_cast<uint16_t>(ca + va + (cc & 1));
            ca = static_cast<uint8_t>(suma);
            const uint8_t vaX = static_cast<uint8_t>(va ^ cc);
            const uint16_t sumb = static_cast<uint16_t>(cb + vb + (suma >> 8));
            cb = static_cast<uint8_t>(sumb);
            const uint8_t vbX = static_cast<uint8_t>(vb ^ ca);
            if (i != 174) {
                cc = static_cast<uint8_t>(cc + vc + (sumb >> 8));
            }
            const uint8_t vcX = static_cast<uint8_t>(vc ^ cb);

            const uint32_t nb = (i != 174) ? 32u : 24u;
            const uint32_t enc = gcr6Encode(vaX, vbX, vcX);
            rawW(cells, static_cast<int>(nb), enc >> (32 - nb));
        }
        // Running checksum (3 bytes) packed as 4 GCR bytes.
        rawW(cells, 32, gcr6Encode(ca, cb, cc));
        // Data epilogue + pad (MAME 2102).
        rawW(cells, 32, 0xdeaaffff);
    }

    // The pregap was sized so the sum of pregap + N × 6208 equals
    // cellsRev exactly — but the prepregap rounding above can leave
    // the stream a few cells shy. Pad with self-sync if needed; trim
    // if a rounding accident overshot.
    while (static_cast<int>(cells.size()) < cellsRev) {
        cells.push_back(1);     // pad bit (single $FF cell)
    }
    if (static_cast<int>(cells.size()) > cellsRev) {
        cells.resize(static_cast<size_t>(cellsRev));
    }
}

}  // namespace

Sony35Drive::Sony35Drive()
{
    reset();
}

bool Sony35Drive::isInserted() const
{
    return image_ && image_->isLoaded();
}

int Sony35Drive::cellsPerRev() const
{
    return kCellsPerRev[zoneForTrack(track_)];
}

int64_t Sony35Drive::cyclesPerRev() const
{
    return kCyclesPerRev[zoneForTrack(track_)];
}

void Sony35Drive::invalidateCache() const
{
    cachedTrack_ = -1;
    cachedHead_  = -1;
    cells_.clear();
    transitionCells_.clear();
    cachedCellsPerRev_   = 0;
    cachedCyclesPerRev_  = 0;
}

void Sony35Drive::rebuildTransitionsFromCells() const
{
    transitionCells_.clear();
    transitionCells_.reserve(cells_.size() / 4);
    for (int i = 0; i < static_cast<int>(cells_.size()); ++i) {
        if (cells_[i]) transitionCells_.push_back(i);
    }
}

std::vector<uint8_t> Sony35Drive::debugCellStream() const
{
    if (!isInserted() || track_ < 0 || track_ >= 80) return {};
    ensureCache();
    return cells_;
}

namespace {

// Verbatim port of MAME `flopimg.cpp:1530-1569 generate_nibbles_from
// _bitstream`. The IWM is a self-clocking GCR data separator: it
// shifts in cells MSB-first and emits an 8-bit "disk byte" once the
// top of the shift register is 1 (= the most recent transition was 8
// cells ago). MAME's algorithm:
//
//   1. Initial alignment: walk forward across the entire bitstream,
//      jumping `pos += 8` from each 1-bit. Whatever cell `pos` lands
//      on after wrapping is "where the byte boundary is" relative to
//      cell 0 of the buffer.
//   2. Reader loop: read 8 cells from `pos` (with wrap), emit a byte,
//      then skip trailing zero cells (the self-sync gap stretches
//      $FF runs by 2 cells per byte).
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

}  // namespace

int Sony35Drive::decodeAndCommit() const
{
    // Port of MAME `flopimg.cpp:2107 extract_sectors_from_track_mac_gcr6`.
    // Walks the nibblised cell stream looking for D5AA96 address
    // fields and matching D5AAAD data fields; decoded sectors are
    // written back to the attached `Disk35Image` block at the index
    // computed from (track, head, logicalSec).
    if (!image_ || !image_->isLoaded()) return 0;
    if (cells_.empty()) return 0;
    if (track_ < 0 || track_ >= 80) return 0;
    const int head    = side1_ ? 1 : 0;
    const int sectors = sectorsForTrack35(track_);

    auto nib = nibblesFromCells(cells_);
    if (nib.size() < 300) return 0;

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
        if (chk != c1 || se >= sectors ||
            h[5] != 0xDE || h[6] != 0xAA) continue;
        if (static_cast<int>(tr) != track_) continue;

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
        bool decodeOk = true;
        for (int i = 0; i < 175 && decodeOk; ++i) {
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
        if (!decodeOk) continue;

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
        const int blkIdx = blockIndexFor(track_, head, se);
        if (blkIdx < 0 ||
            blkIdx >= static_cast<int>(Disk35Image::kBlockCount)) {
            continue;
        }
        // Only write if the block actually differs — avoids dirtying
        // the image when the firmware just re-writes the same data.
        uint8_t existing[Disk35Image::kBlockBytes];
        if (image_->readBlock(blkIdx, existing) &&
            std::memcmp(existing, sdata + 12,
                        Disk35Image::kBlockBytes) == 0) {
            continue;
        }
        if (image_->writeBlock(blkIdx, sdata + 12)) {
            ++written;
        }
    }
    return written;
}

void Sony35Drive::writeFlux(int64_t startCpu, int64_t endCpu,
                            const int64_t* fluxes, int count,
                            int64_t revStart)
{
    if (!isInserted() || track_ < 0 || track_ >= 80) return;
    if (image_->isWriteProtected()) return;
    ensureCache();
    if (cells_.empty() || cachedCellsPerRev_ <= 0 ||
        cachedCyclesPerRev_ <= 0) {
        return;
    }
    if (endCpu <= startCpu) return;

    const int     n      = cachedCellsPerRev_;
    const int64_t period = cachedCyclesPerRev_;

    // Map CPU cycle → cell index inside one revolution (anchored on
    // `revStart`). Negative offsets wrap forward; offsets ≥ period
    // wrap modulo. The cell index for a transition at cycle T is the
    // *floor* of (T - revStart) / period × n — the nearest cell-time
    // earlier than or equal to T.
    // The encoder uses floor-division when computing `cycleForCell(i)
    // = i × period / n` for the flux time stamp; integer truncation
    // means cell 1's stamp lands at floor(2.024) = 2 even though the
    // true cell midpoint is 3.04 cycles later. To recover the same
    // cell index from a flux timestamp we round-to-nearest here —
    // floor would push every transition one cell earlier and lose
    // address-field markers.
    auto cycleToCell = [&](int64_t cy) -> int {
        int64_t rel = cy - revStart;
        rel %= period;
        if (rel < 0) rel += period;
        int cell = static_cast<int>(
            (rel * n + period / 2) / period);
        if (cell >= n) cell -= n;
        return cell;
    };

    const int cellStart = cycleToCell(startCpu);
    const int cellEnd   = cycleToCell(endCpu);

    // Clear cells in the write window. The window can wrap around the
    // revolution boundary if the IWM wrote past cell n-1.
    if (cellStart == cellEnd && endCpu - startCpu >= period) {
        // Full-track rewrite — clobber everything.
        std::fill(cells_.begin(), cells_.end(), 0);
    } else if (cellStart <= cellEnd) {
        std::fill(cells_.begin() + cellStart,
                  cells_.begin() + cellEnd, 0);
    } else {
        std::fill(cells_.begin() + cellStart, cells_.end(), 0);
        std::fill(cells_.begin(), cells_.begin() + cellEnd, 0);
    }

    // Splice in the new flux transitions. Each transition lands in
    // the cell containing its cycle stamp.
    for (int i = 0; i < count; ++i) {
        const int cell = cycleToCell(fluxes[i]);
        cells_[cell] = 1;
    }

    rebuildTransitionsFromCells();
    decodeAndCommit();
}

void Sony35Drive::ensureCache() const
{
    const int head = side1_ ? 1 : 0;
    if (cachedTrack_ == track_ && cachedHead_ == head && !cells_.empty()) {
        return;
    }
    cells_.clear();
    transitionCells_.clear();
    cachedCellsPerRev_   = 0;
    cachedCyclesPerRev_  = 0;
    cachedTrack_ = track_;
    cachedHead_  = head;
    if (!isInserted()) return;
    if (track_ < 0 || track_ >= 80) return;

    buildTrackBits(*image_, track_, head, cells_);
    cachedCellsPerRev_  = static_cast<int>(cells_.size());
    cachedCyclesPerRev_ = kCyclesPerRev[zoneForTrack(track_)];
    rebuildTransitionsFromCells();
}

int64_t Sony35Drive::nextTransition(int64_t fromCpuCycle,
                                    int64_t revStart) const
{
    ensureCache();
    if (transitionCells_.empty()) return INT64_MAX;

    const int64_t period = cachedCyclesPerRev_;
    const int     ncells = cachedCellsPerRev_;
    if (period <= 0 || ncells <= 0) return INT64_MAX;

    // Anchor the head: cell 0 was under the head at `revStart`. Find
    // the relative time inside the current revolution.
    int64_t rel = fromCpuCycle - revStart;
    int64_t revIdx = rel >= 0 ? (rel / period) : -((-rel + period - 1) / period);
    int64_t relInRev = rel - revIdx * period;
    if (relInRev < 0) { relInRev += period; --revIdx; }

    // Convert to cell space. We want the next transition STRICTLY
    // after `fromCpuCycle`, so look for cell with `cellTime > relInRev`.
    auto cycleForCell = [period, ncells](int cellIdx) -> int64_t {
        // Round to nearest CPU cycle. period × cellIdx might overflow
        // 32 bits but stays inside 64-bit (period ≤ ~156k, ncells ≤ ~77k).
        return (static_cast<int64_t>(cellIdx) * period) / ncells;
    };

    // Binary-search the transition list for the first cell whose
    // cycle-time exceeds relInRev.
    int lo = 0, hi = static_cast<int>(transitionCells_.size());
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (cycleForCell(transitionCells_[mid]) > relInRev) hi = mid;
        else                                                lo = mid + 1;
    }

    if (lo < static_cast<int>(transitionCells_.size())) {
        return revStart + revIdx * period + cycleForCell(transitionCells_[lo]);
    }
    // No transition left in this revolution — first event of the next.
    return revStart + (revIdx + 1) * period + cycleForCell(transitionCells_[0]);
}

void Sony35Drive::reset()
{
    motorOn_       = false;
    writeProtect_  = true;
    side1_         = false;
    sel_           = false;
    directionIn_   = true;
    diskSwitched_  = false;
    track_         = 0;
    phases_        = 0;
    prevPhases_    = 0;
    if (image_) {
        writeProtect_ = image_->isWriteProtected();
    }
}

void Sony35Drive::setImage(Disk35Image* image)
{
    // image_ is the drive *slot* — a stable Disk35Image instance owned
    // by EmulationController. Mounting / ejecting media is a separate
    // event signalled via `notifyMediaChange()`.
    image_ = image;
    writeProtect_ = image && image->isWriteProtected();
    invalidateCache();
}

void Sony35Drive::notifyMediaChange()
{
    diskSwitched_ = true;
    writeProtect_ = image_ && image_->isWriteProtected();
    invalidateCache();
    pom2::log().info(
        "Sony35",
        std::string("media change ") +
            ((image_ && image_->isLoaded()) ? image_->path() : "(empty)"));
}

void Sony35Drive::monW(bool motorOffHigh)
{
    // MAME: m_floppy->mon_w(true) = motor STOP. The IWM calls this when
    // it leaves MODE_ACTIVE.
    motorOn_ = !motorOffHigh;
}

void Sony35Drive::ssW(bool side1)
{
    if (side1_ != side1) {
        side1_ = side1;
        invalidateCache();
    }
}

void Sony35Drive::setSel(bool sel)
{
    sel_ = sel;
}

uint8_t Sony35Drive::regSelect() const
{
    // { SEL, CA2, CA1, CA0 }
    uint8_t r = (phases_ & (kBitCA2 | kBitCA1 | kBitCA0));
    if (sel_) r |= 0x08;
    return r;
}

void Sony35Drive::seekPhaseW(uint8_t phases)
{
    // MAME `mac_floppy.cpp::seek_phase_w`: latch the new phase bits,
    // then if LSTRB transitioned 0→1 fire `strobeWriteRegister` with
    // the current `regSelect()` address. The IWM is free to change
    // CA0/CA1/CA2 while LSTRB is held — MAME only fires the strobe on
    // the rising edge.
    prevPhases_ = phases_;
    phases_     = static_cast<uint8_t>(phases & 0x0F);
    const bool lstrbWasLow = !(prevPhases_ & kBitLSTRB);
    const bool lstrbNowHi  =  (phases_     & kBitLSTRB);
    if (lstrbWasLow && lstrbNowHi) {
        strobeWriteRegister(regSelect());
    }
}

void Sony35Drive::strobeWriteRegister(uint8_t reg)
{
    // Decode per the table at the top of this file. Effects on POM2's
    // internal state machine are the minimum needed for the //c+
    // SmartPort probe; finer-grained behaviour (step debouncing, eject
    // animation, RPM ramp-up) is deferred.
    switch (reg) {
        case 0x0: directionIn_ = true;  break;       // direction inward
        case 0x1:                                     // step
            if (directionIn_ && track_ > 0)  { --track_; invalidateCache(); }
            if (!directionIn_ && track_ < 79) { ++track_; invalidateCache(); }
            break;
        case 0x2: motorOn_ = true;  break;            // motor on
        case 0x3: motorOn_ = false; break;            // motor off
        case 0x4:                                     // eject
            if (image_ && image_->isLoaded()) {
                image_->eject();
                diskSwitched_ = true;
                pom2::log().info("Sony35", "eject requested by host");
            }
            break;
        case 0x8: side1_ = false; break;              // head 0
        case 0x9: side1_ = true;  break;              // head 1
        default:
            // Unmapped register — MAME logs but does nothing.
            break;
    }
}

bool Sony35Drive::senseR() const
{
    // Active-low logic on the SENSE line. Each register returns 1
    // (HIGH) for "condition NOT asserted". Lines marked "reserved"
    // return 1 (MAME also returns the default 1 for those).
    const uint8_t reg = regSelect();
    switch (reg) {
        case 0x0:                                       // /DIRTN
            return !directionIn_;
        case 0x1:                                       // /STEP — 1 = step done
            return true;
        case 0x2:                                       // /MOTOR ON — 0 when on
            return !motorOn_;
        case 0x3:                                       // /TRACK0 — 0 at trk 0
            return track_ != 0;
        case 0x4:                                       // /SWITCHED — 0 = just switched
            // Latching flip-flop: stays 0 until read once, then snaps
            // back to 1. The //c+ firmware uses this to drive its
            // "media changed" SmartPort status.
            if (diskSwitched_) {
                // MAME clears the latch on read; we mirror that. The
                // const-cast is a controlled exception — `senseR` is
                // logically a state-changing read on real hardware.
                const_cast<Sony35Drive*>(this)->diskSwitched_ = false;
                return false;
            }
            return true;
        case 0x6:                                       // /TACH
            // Reserved on stock 800K drive; 1 = no tach pulse.
            return true;
        case 0x8:                                       // /SIDES — 0 = double-sided
            return false;                               // 800K Sony is always 2-sided
        case 0xA:                                       // /READY — 0 = ready
            return !(image_ && image_->isLoaded() && motorOn_);
        case 0xB:                                       // /INSERTED — 0 = disk in
            return !(image_ && image_->isLoaded());
        case 0xD:                                       // /SEL
            return !sel_;
        case 0xF:                                       // /DRVIN — 0 = drive present
            return false;                               // present
        default:
            return true;                                // reserved → high
    }
}

}  // namespace pom2

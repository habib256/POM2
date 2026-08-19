// WOZ 3.5" load test — pins `Disk35Image`'s flux path.
//
// POM2 stores 3.5" media as a flat block array and has no GCR *encoder*, so
// a `.woz` (which holds bit cells, not blocks) has nothing to be mounted as
// unless it is decoded at LOAD time. That decode is `Sony35Gcr`, shared with
// `Sony35Drive::decodeAndCommit` — the drive reaches the same cells from the
// other side, when the guest writes a track.
//
// The oracle here is a SECOND, INDEPENDENT encoder, written against MAME
// `flopimg.cpp:2017 build_mac_track_gcr` rather than reusing POM2's: a test
// that shared the decoder's tables could not catch a bad table, and the
// table is exactly the sort of thing that is transcribed wrong once and
// believed forever.
//
// Pinned:
//   1. Round trip — blocks encoded into a synthetic WOZ2 come back byte
//      for byte, on both heads and across a zone boundary (the sector count
//      per track steps 12/11/10/9/8).
//   2. A WOZ mounts WRITE-PROTECTED. Giving blocks back would mean
//      re-encoding the user's flux, which POM2 cannot do; silently
//      accepting writes and dropping them at flush is the failure this
//      forecloses.
//   3. The refusals are specific: a 5.25" WOZ says so (mount it as a Disk
//      II image) and a WOZ1 says so, rather than "not an 800K image".

#include "Disk35Image.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ─── An independent Sony 800K GCR encoder (MAME build_mac_track_gcr) ──────

const uint8_t kFw[0x40] = {
    0x96, 0x97, 0x9a, 0x9b, 0x9d, 0x9e, 0x9f, 0xa6,
    0xa7, 0xab, 0xac, 0xad, 0xae, 0xaf, 0xb2, 0xb3,
    0xb4, 0xb5, 0xb6, 0xb7, 0xb9, 0xba, 0xbb, 0xbc,
    0xbd, 0xbe, 0xbf, 0xcb, 0xcd, 0xce, 0xcf, 0xd3,
    0xd6, 0xd7, 0xd9, 0xda, 0xdb, 0xdc, 0xdd, 0xde,
    0xdf, 0xe5, 0xe6, 0xe7, 0xe9, 0xea, 0xeb, 0xec,
    0xed, 0xee, 0xef, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6,
    0xf7, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

int sectorsFor(int track) { return 12 - (track / 16); }

int blockIndex(int track, int head, int sec)
{
    int idx = 0;
    for (int t = 0; t < track; ++t) idx += 2 * sectorsFor(t);
    return idx + head * sectorsFor(track) + sec;
}

struct BitWriter {
    std::vector<uint8_t> bits;                       // one entry per cell
    void raw(int n, uint32_t v) {
        for (int i = n - 1; i >= 0; --i) bits.push_back((v >> i) & 1);
    }
};

uint32_t enc3(uint8_t va, uint8_t vb, uint8_t vc)
{
    return (uint32_t(kFw[((va >> 2) & 0x30) | ((vb >> 4) & 0x0c) |
                        ((vc >> 6) & 0x03)]) << 24) |
           (uint32_t(kFw[va & 0x3f]) << 16) |
           (uint32_t(kFw[vb & 0x3f]) <<  8) |
           (uint32_t(kFw[vc & 0x3f]));
}

/// Encode one (track, head) as a cell stream. `sectorBytes` is
/// sectorsFor(track) * 512, in LOGICAL sector order.
std::vector<uint8_t> encodeTrack(int track, int head, const uint8_t* sectorBytes)
{
    BitWriter w;
    const int sectors = sectorsFor(track);
    for (int s = 0; s < sectors; ++s) {
        for (int i = 0; i < 8; ++i) { w.raw(24, 0xff3fcf); w.raw(24, 0xf3fcff); }

        w.raw(24, 0xd5aa96);
        const uint8_t tr  = uint8_t(track);
        const uint8_t sec = uint8_t(s);
        const uint8_t sid = uint8_t(((tr & 0x40) ? 1 : 0) | (head ? 0x20 : 0));
        const uint8_t fmt = 0x22;
        const uint8_t chk = uint8_t(tr ^ sec ^ sid ^ fmt);
        w.raw(8, kFw[tr  & 0x3f]);
        w.raw(8, kFw[sec & 0x3f]);
        w.raw(8, kFw[sid & 0x3f]);
        w.raw(8, kFw[fmt & 0x3f]);
        w.raw(8, kFw[chk & 0x3f]);
        w.raw(24, 0xdeaaff);

        w.raw(24, 0xff3fcf); w.raw(24, 0xf3fcff);

        w.raw(24, 0xd5aaad);
        w.raw(8, kFw[sec & 0x3f]);

        uint8_t tagged[12 + 512 + 1] = {};
        std::memcpy(tagged + 12, sectorBytes + s * 512, 512);

        uint8_t ca = 0, cb = 0, cc = 0;
        for (int i = 0; i < 175; ++i) {
            const uint8_t va = tagged[3*i], vb = tagged[3*i+1];
            const uint8_t vc = (i != 174) ? tagged[3*i+2] : 0;
            cc = uint8_t((cc << 1) | (cc >> 7));
            const uint16_t suma = uint16_t(ca + va + (cc & 1));
            ca = uint8_t(suma);
            const uint8_t vaX = uint8_t(va ^ cc);
            const uint16_t sumb = uint16_t(cb + vb + (suma >> 8));
            cb = uint8_t(sumb);
            const uint8_t vbX = uint8_t(vb ^ ca);
            if (i != 174) cc = uint8_t(cc + vc + (sumb >> 8));
            const uint8_t vcX = uint8_t(vc ^ cb);
            const uint32_t nb = (i != 174) ? 32u : 24u;
            w.raw(int(nb), enc3(vaX, vbX, vcX) >> (32 - nb));
        }
        w.raw(32, enc3(ca, cb, cc));
        w.raw(32, 0xdeaaffff);
    }
    return w.bits;
}

void put16(std::vector<uint8_t>& v, uint16_t x)
{ v.push_back(uint8_t(x)); v.push_back(uint8_t(x >> 8)); }
void put32(std::vector<uint8_t>& v, uint32_t x)
{ for (int i = 0; i < 4; ++i) v.push_back(uint8_t(x >> (8 * i))); }

/// Wrap per-(track,head) cell streams in a WOZ2 container.
std::vector<uint8_t> buildWoz(const std::vector<std::vector<uint8_t>>& tracks,
                              uint8_t diskType = 2, char version = '2')
{
    std::vector<uint8_t> woz = { 'W', 'O', 'Z', uint8_t(version),
                                 0xFF, 0x0A, 0x0D, 0x0A };
    put32(woz, 0);                                   // CRC (unchecked)

    std::vector<uint8_t> info(60, 0);
    info[0] = 2;                                     // INFO version
    info[1] = diskType;
    info[37] = 2;                                    // sides
    woz.insert(woz.end(), { 'I','N','F','O' }); put32(woz, uint32_t(info.size()));
    woz.insert(woz.end(), info.begin(), info.end());

    std::vector<uint8_t> tmap(160, 0xFF);
    for (size_t i = 0; i < tracks.size() && i < 160; ++i)
        if (!tracks[i].empty()) tmap[i] = uint8_t(i);
    woz.insert(woz.end(), { 'T','M','A','P' }); put32(woz, 160);
    woz.insert(woz.end(), tmap.begin(), tmap.end());

    // TRKS: 160 entries, then the bit blocks. Entries reference 512-byte
    // blocks counted from the START OF FILE, so lay the data out first.
    std::vector<uint8_t> entries, blob;
    const size_t trksHeaderBytes = 8 + 160 * 8;
    const size_t dataStart = woz.size() + trksHeaderBytes;
    size_t firstBlock = (dataStart + 511) / 512;
    // Pad so the first bit block lands on a 512 boundary.
    for (size_t i = 0; i < 160; ++i) {
        if (i >= tracks.size() || tracks[i].empty()) {
            put16(entries, 0); put16(entries, 0); put32(entries, 0);
            continue;
        }
        const uint32_t nbits = uint32_t(tracks[i].size());
        std::vector<uint8_t> packed((nbits + 7) / 8, 0);
        for (uint32_t b = 0; b < nbits; ++b)
            if (tracks[i][b]) packed[b >> 3] |= uint8_t(0x80 >> (b & 7));
        const uint32_t blocks = uint32_t((packed.size() + 511) / 512);
        put16(entries, uint16_t(firstBlock));
        put16(entries, uint16_t(blocks));
        put32(entries, nbits);
        packed.resize(size_t(blocks) * 512, 0);
        blob.insert(blob.end(), packed.begin(), packed.end());
        firstBlock += blocks;
    }
    woz.insert(woz.end(), { 'T','R','K','S' });
    put32(woz, uint32_t(entries.size() + blob.size()));
    woz.insert(woz.end(), entries.begin(), entries.end());
    woz.resize(((woz.size() + 511) / 512) * 512, 0);   // pad to the block grid
    woz.insert(woz.end(), blob.begin(), blob.end());
    return woz;
}

fs::path writeTemp(const char* name, const std::vector<uint8_t>& bytes)
{
    const fs::path p = fs::temp_directory_path() / name;
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            std::streamsize(bytes.size()));
    return p;
}

// A recognisable payload for block `bi`.
void fillBlock(uint8_t* out, int bi)
{
    for (int i = 0; i < 512; ++i)
        out[i] = uint8_t((bi * 7 + i * 3 + (i >> 4)) & 0xFF);
}

void testRoundTrip()
{
    // Track 0 (zone 0, 12 sectors) and track 16 (zone 1, 11) — a zone
    // boundary — on both heads.
    const int kTracks[] = { 0, 16 };
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<std::pair<int, std::vector<uint8_t>>> expect;

    for (int t : kTracks)
        for (int head = 0; head < 2; ++head) {
            const int n = sectorsFor(t);
            std::vector<uint8_t> payload(size_t(n) * 512);
            for (int s = 0; s < n; ++s) {
                const int bi = blockIndex(t, head, s);
                fillBlock(payload.data() + s * 512, bi);
                expect.push_back({ bi, std::vector<uint8_t>(
                    payload.begin() + s * 512, payload.begin() + (s + 1) * 512) });
            }
            tracks[t * 2 + head] = encodeTrack(t, head, payload.data());
        }

    const fs::path p = writeTemp("pom2_woz35_roundtrip.woz", buildWoz(tracks));
    pom2::Disk35Image img;
    const bool ok = img.loadFile(p.string());
    assert(ok && "a synthetic 3.5\" WOZ2 must mount");
    assert(img.kind() == pom2::Disk35Image::ImageKind::Woz35);

    for (const auto& [bi, want] : expect) {
        uint8_t got[512];
        assert(img.readBlock(uint32_t(bi), got));
        assert(std::memcmp(got, want.data(), 512) == 0 &&
               "every encoded sector must decode back byte for byte");
    }
    // Tracks the WOZ never described read as zeros rather than garbage.
    uint8_t blank[512];
    assert(img.readBlock(800, blank));
    bool allZero = true;
    for (uint8_t v : blank) if (v) { allZero = false; break; }
    assert(allZero);

    std::error_code ec; fs::remove(p, ec);
    std::printf("  ok: %zu sectors round-trip through a synthetic WOZ2\n",
                expect.size());
}

void testWriteProtected()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<uint8_t> payload(12 * 512);
    for (int s = 0; s < 12; ++s) fillBlock(payload.data() + s * 512, s);
    tracks[0] = encodeTrack(0, 0, payload.data());

    const fs::path p = writeTemp("pom2_woz35_wp.woz", buildWoz(tracks));
    pom2::Disk35Image img;
    assert(img.loadFile(p.string()));
    // Even with write-back explicitly enabled: there is no GCR encoder, so
    // handing blocks back to a .woz is not something POM2 can honour.
    img.setWriteBackEnabled(true);
    assert(img.isWriteProtected());
    uint8_t junk[512] = {};
    assert(!img.writeBlock(0, junk));
    assert(!img.hasUnsavedChanges());

    std::error_code ec; fs::remove(p, ec);
    std::printf("  ok: a WOZ mounts read-only, writes refused\n");
}

// The way OUT of the read-only WOZ above: convert to a .po. Pinned because
// the whole point is that the copy is byte-identical AND writable — a
// conversion that quietly dropped a block, or produced another read-only
// image, would look like it worked.
void testConvertToPo()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<std::pair<int, std::vector<uint8_t>>> expect;
    for (int t : { 0, 16 })
        for (int head = 0; head < 2; ++head) {
            const int n = sectorsFor(t);
            std::vector<uint8_t> payload(size_t(n) * 512);
            for (int s = 0; s < n; ++s) {
                const int bi = blockIndex(t, head, s);
                fillBlock(payload.data() + s * 512, bi);
                expect.push_back({ bi, std::vector<uint8_t>(
                    payload.begin() + s * 512, payload.begin() + (s + 1) * 512) });
            }
            tracks[t * 2 + head] = encodeTrack(t, head, payload.data());
        }

    const fs::path p = writeTemp("pom2_woz35_convert.woz", buildWoz(tracks));
    const fs::path out = fs::temp_directory_path() / "pom2_woz35_convert.po";
    std::error_code ec; fs::remove(out, ec);

    pom2::Disk35Image woz;
    assert(woz.loadFile(p.string()));
    std::string err;
    assert(woz.exportRawTo(out.string(), err) && "export must succeed");
    assert(fs::file_size(out, ec) == pom2::Disk35Image::kBytesPerImage &&
           "a converted 3.5\" image is exactly 819200 bytes");

    // Never clobber: a second export to the same name must refuse rather
    // than replace a disk the user may have been working in.
    assert(!woz.exportRawTo(out.string(), err));
    assert(err.find("already exists") != std::string::npos);

    pom2::Disk35Image po;
    assert(po.loadFile(out.string()));
    assert(po.kind() == pom2::Disk35Image::ImageKind::Raw800k);
    for (const auto& [bi, want] : expect) {
        uint8_t got[512];
        assert(po.readBlock(uint32_t(bi), got));
        assert(std::memcmp(got, want.data(), 512) == 0 &&
               "the .po must carry the WOZ's decoded blocks unchanged");
    }
    // And, unlike its source, it takes writes once the user opts in.
    po.setWriteBackEnabled(true);
    assert(!po.isWriteProtected());
    uint8_t mark[512];
    std::memset(mark, 0xA5, sizeof(mark));
    assert(po.writeBlock(9, mark));
    assert(po.saveDirty());
    pom2::Disk35Image again;
    assert(again.loadFile(out.string()));
    uint8_t got[512];
    assert(again.readBlock(9, got));
    assert(std::memcmp(got, mark, 512) == 0 && "the write must be durable");

    fs::remove(p, ec); fs::remove(out, ec);
    std::printf("  ok: WOZ converts to an identical, writable .po\n");
}

void testRefusals()
{
    std::vector<std::vector<uint8_t>> tracks(160);
    std::vector<uint8_t> payload(12 * 512, 0);
    tracks[0] = encodeTrack(0, 0, payload.data());

    {   // 5.25" WOZ — belongs to DiskImage, and the message must say so.
        const fs::path p = writeTemp("pom2_woz35_525.woz",
                                     buildWoz(tracks, /*diskType=*/1));
        pom2::Disk35Image img;
        assert(!img.loadFile(p.string()));
        assert(img.lastError().find("5.25") != std::string::npos);
        std::error_code ec; fs::remove(p, ec);
    }
    {   // WOZ1 — a different TRKS shape, not decoded.
        const fs::path p = writeTemp("pom2_woz35_v1.woz",
                                     buildWoz(tracks, 2, /*version=*/'1'));
        pom2::Disk35Image img;
        assert(!img.loadFile(p.string()));
        assert(img.lastError().find("WOZ1") != std::string::npos);
        std::error_code ec; fs::remove(p, ec);
    }
    std::printf("  ok: 5.25\" and WOZ1 images are refused by name\n");
}

}  // namespace

int main()
{
    std::printf("woz35_load\n");
    testRoundTrip();
    testWriteProtected();
    testConvertToPo();
    testRefusals();
    std::printf("PASS\n");
    return 0;
}

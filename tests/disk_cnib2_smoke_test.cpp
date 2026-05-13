// Pins the CNib2 (6384-byte/track NIB variant) detection + load path.
//
// CNib2 is a rarer NIB encoding where each track is 6384 nibbles on disk
// instead of the standard 6656. AppleWin recognises it as a distinct
// class; we accept the 223 440-byte total size and pad each track up to
// the 6656-wide runtime buffer with $FF (sync gap). After load:
//   - tracks[t][0 .. 6383]    = source bytes
//   - tracks[t][6384 .. 6655] = $FF (the inserted sync pad)
// The padding lets the LSS see a normal sync run at the wrap-around
// point so existing controller code (which assumes 6656-wide tracks)
// stays oblivious.

#include "DiskImage.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int kSrcBytesPerTrack = 6384;
constexpr int kCNib2TotalBytes  = DiskImage::kTracks * kSrcBytesPerTrack;

bool runSyntheticLoad()
{
    // Pattern: byte = (track ^ position) so an off-by-one in track
    // indexing OR a padding/offset bug both surface immediately.
    std::vector<uint8_t> buf(static_cast<std::size_t>(kCNib2TotalBytes));
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        for (int i = 0; i < kSrcBytesPerTrack; ++i) {
            buf[static_cast<std::size_t>(t) * kSrcBytesPerTrack + i] =
                static_cast<uint8_t>((t * 13 + i) & 0xFF);
        }
    }

    const std::string path = "cnib2_synthetic.nib";
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) {
        std::fprintf(stderr, "synthetic: cannot create temp file\n");
        return false;
    }
    wf.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "synthetic: loadFile failed: %s\n",
                     img.getLastError().c_str());
        return false;
    }

    // The runtime track buffer width is kNibblesPerTrack=6656. Spot-check
    // the first byte of every track and the last source byte (index 6383)
    // plus the first padding byte (index 6384, must be $FF).
    for (int t = 0; t < DiskImage::kTracks; ++t) {
        const uint8_t firstExpected = static_cast<uint8_t>((t * 13 + 0) & 0xFF);
        const uint8_t lastSrcExpected =
            static_cast<uint8_t>((t * 13 + 6383) & 0xFF);
        const uint8_t firstByte = img.nibbleAt(t, 0);
        const uint8_t lastSrcByte = img.nibbleAt(t, 6383);
        const uint8_t firstPadByte = img.nibbleAt(t, 6384);
        const uint8_t lastPadByte =
            img.nibbleAt(t, DiskImage::kNibblesPerTrack - 1);
        if (firstByte != firstExpected) {
            std::fprintf(stderr,
                "synthetic: track %d byte 0 = 0x%02X, want 0x%02X\n",
                t, firstByte, firstExpected);
            return false;
        }
        if (lastSrcByte != lastSrcExpected) {
            std::fprintf(stderr,
                "synthetic: track %d byte 6383 = 0x%02X, want 0x%02X\n",
                t, lastSrcByte, lastSrcExpected);
            return false;
        }
        if (firstPadByte != 0xFF || lastPadByte != 0xFF) {
            std::fprintf(stderr,
                "synthetic: track %d padding wrong "
                "(byte 6384=0x%02X, last=0x%02X) — both must be $FF\n",
                t, firstPadByte, lastPadByte);
            return false;
        }
    }
    return true;
}

// Sanity: a 223 440-byte file with bytes not matching anything special
// is recognised by SIZE alone — no extension required. This catches the
// "rename .nib → .x" case where the extension lies but the content is OK.
bool runSizeOnlyDetection()
{
    std::vector<uint8_t> buf(static_cast<std::size_t>(kCNib2TotalBytes), 0xAA);
    const std::string path = "cnib2_no_ext_hint";   // deliberately no .nib
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) return false;
    wf.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "size-only: loadFile refused %s: %s\n",
                     path.c_str(), img.getLastError().c_str());
        return false;
    }
    if (img.nibbleAt(0, 0) != 0xAA) {
        std::fprintf(stderr,
            "size-only: track 0 byte 0 = 0x%02X, want 0xAA\n",
            img.nibbleAt(0, 0));
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= runSyntheticLoad();
    ok &= runSizeOnlyDetection();
    if (!ok) return 1;
    std::printf("disk_cnib2_smoke OK: 35 × 6384 → padded 35 × 6656\n");
    return 0;
}

// WOZ2 writeFlux cell-width regression test.
//
// DiskImage::writeFlux converts incoming flux-transition timestamps (in
// LSS cycles) back into bit cells. That conversion MUST use the same cell
// width as the read path (expandTrackFlux: cell i is centred at
// `i*lssCyclesPerCell()`), i.e. `cell = t / lssCyclesPerCell()`.
//
// Regression: writeFlux hard-coded the divisor to 8. For a WOZ2 image
// whose `optimal_bit_timing` is the default 32 (cyc = 8) that happens to
// be correct, so the existing woz_writeback test (which fixes info[39]=32)
// never caught it. But for a slower master (obt=40 → cyc=10), `/8`
// scatters every written transition into the wrong cell, silently
// corrupting the track on write-back.
//
// This test builds a WOZ2 with obt=40, writes a single transition that
// should land in cell 100, and verifies the round-trip flux event comes
// back at cell 100's centre (1005) — not cell 125 (1255), which is where
// the `/8` bug parked it. An obt=32 control proves the default path is
// unchanged.

#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;

void putU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}
void putU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
}

// Minimal WOZ2, track 0, all-zero bit cells, settable optimal_bit_timing
// and bit_count. (Same shape as woz_bit_timing_smoke_test, inlined so
// this TU is independent in CI bisects.)
std::vector<uint8_t> buildWoz2(uint8_t optimalBitTiming, uint32_t bitCount) {
    std::vector<uint8_t> woz;
    woz.insert(woz.end(), {'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);                                 // CRC32 (unchecked)

    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    std::vector<uint8_t> info(60, 0);
    info[0]  = 2;                                     // info_version 2
    info[1]  = 1;                                     // disk_type 5.25"
    info[2]  = 0;                                     // write_protected = no
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    info[37] = 1;                                     // disk_sides
    info[39] = optimalBitTiming;                      // ← field under test
    addChunk("INFO", info);

    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;                                      // qt 0 → TRKS entry 0
    addChunk("TMAP", tmap);

    std::vector<uint8_t> trks;
    putU16LE(trks, 3);                               // starting_block
    putU16LE(trks, 1);                               // block_count
    putU32LE(trks, bitCount);                        // bit_count
    while (trks.size() < 160 * 8) trks.push_back(0);
    while (trks.size() < (1536 - 12 - 8 - 60 - 8 - 160 - 8)) trks.push_back(0);
    std::vector<uint8_t> blockData(512, 0);          // all-zero track
    trks.insert(trks.end(), blockData.begin(), blockData.end());
    addChunk("TRKS", trks);
    return woz;
}

std::string writeTemp(const std::vector<uint8_t>& bytes, const char* tag) {
    const auto p = fs::temp_directory_path()
        / (std::string("pom2_woz_wf_") + tag + ".woz");
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WOZ");
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return p.string();
}

// Write one transition that belongs in `targetCell`, then confirm the
// round-trip flux event comes back at that cell's centre.
bool checkRoundTrip(uint8_t obt, int targetCell) {
    const uint32_t bitCount = 200;                   // 200 cells
    const auto woz  = buildWoz2(obt, bitCount);
    const auto path = writeTemp(woz, std::to_string(obt).c_str());

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load obt=%u: %s\n",
                    static_cast<unsigned>(obt), img.getLastError().c_str());
        return false;
    }
    const int cyc    = obt / 4;                       // LSS cycles per cell
    const int period = img.trackPeriod(0);            // = bitCount * cyc
    assert(period == static_cast<int>(bitCount) * cyc);

    // A transition at the centre of `targetCell`.
    const int64_t ts = static_cast<int64_t>(targetCell) * cyc + cyc / 2;
    int64_t transitions[1] = { ts };
    img.writeFlux(0, /*start*/ 0, /*end*/ period, /*count*/ 1, transitions);

    // writeFlux invalidates the flux cache; fluxEvents regenerates it from
    // the bit stream we just spliced. The single set cell must reappear.
    const auto& flux = img.fluxEvents(0);
    const int expected = targetCell * cyc + cyc / 2;
    if (flux.size() != 1 || flux[0] != expected) {
        std::printf("FAIL: obt=%u cell=%d → flux.size=%zu flux[0]=%d "
                    "(expected exactly {%d})\n",
                    static_cast<unsigned>(obt), targetCell, flux.size(),
                    flux.empty() ? -1 : flux[0], expected);
        return false;
    }
    // Direct bit-stream check: the right cell is set, the /8-bug cell isn't.
    if (img.bitAt(0, targetCell) != 1) {
        std::printf("FAIL: obt=%u bitAt(cell %d)=0\n",
                    static_cast<unsigned>(obt), targetCell);
        return false;
    }
    std::printf("OK : obt=%u cyc=%d → cell %d round-trips to flux %d\n",
                static_cast<unsigned>(obt), cyc, targetCell, flux[0]);
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= checkRoundTrip(32, 100);   // control: default timing (cyc=8)
    ok &= checkRoundTrip(40, 100);   // the regression: cyc=10 (was /8 → cell 125)
    ok &= checkRoundTrip(28, 100);   // faster master: cyc=7
    if (ok) std::printf("OK woz_writeflux_smoke\n");
    return ok ? 0 : 1;
}

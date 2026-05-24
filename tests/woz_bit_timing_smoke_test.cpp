// WOZ2 optimal_bit_timing smoke test.
//
// Pins the INFO+39 read in DiskImage::loadWoz against the flux-event
// view: the spacing between two consecutive flux pulses must scale with
// `optimal_bit_timing` (in 125ns units) divided by 4 (LSS clock = 2 MHz
// → 1 LSS cycle = 500ns = 4 × 125ns). At the default value of 32 (= 4µs
// cells) a "1" bit is centred at LSS cycle `i*8 + 4`. With 40 (= 5µs
// cells) the same bit is centred at `i*10 + 5`.
//
// We build three minimal WOZ2 images differing only in INFO[39]:
//   - 32 (default 5.25" timing)
//   - 40 (a +25% slower master, the spec's example for slow disks)
//   - 28 (a -12.5% faster master, e.g. an experimental tightly-packed image)
//
// Each image carries the same bit pattern in track 0 (a single "1" bit
// followed by zeros), and we verify:
//   1. DiskImage::trackPeriod() scales linearly with the timing field.
//   2. The first flux event lands at the expected centre cycle.
//
// Building on the helpers in woz_load_smoke_test.cpp, but kept self-
// contained so this TU compiles independently in CI bisects.

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

// Minimal WOZ2 with `optimal_bit_timing` settable. Track 0 carries
// `bitData` MSB-first; bit_count = bitCount.
std::vector<uint8_t> buildWoz2WithBitTiming(uint8_t optimalBitTiming,
                                            const std::vector<uint8_t>& bitData,
                                            uint32_t bitCount)
{
    std::vector<uint8_t> woz;
    woz.insert(woz.end(),
        {'W', 'O', 'Z', '2', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);                       // CRC32 (unchecked)

    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };

    // INFO (60 bytes; only the fields the loader inspects matter).
    std::vector<uint8_t> info(60, 0);
    info[0]  = 2;                           // info_version 2 (WOZ2)
    info[1]  = 1;                           // disk_type 5.25"
    info[2]  = 0;                           // write_protected
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    info[37] = 1;                           // disk_sides
    info[38] = 0;                           // boot_sector_format (irrelevant)
    info[39] = optimalBitTiming;            // ← the field under test
    addChunk("INFO", info);

    // TMAP (track 0 only).
    std::vector<uint8_t> tmap(160, 0xFF);
    tmap[0] = 0;
    addChunk("TMAP", tmap);

    // TRKS — 160 × 8B headers + 1 block of bit data at file offset 1536.
    std::vector<uint8_t> trks;
    trks.reserve(1280 + 512);
    putU16LE(trks, 3);                      // starting_block
    putU16LE(trks, 1);                      // block_count
    putU32LE(trks, bitCount);               // bit_count (u32 LE)
    while (trks.size() < 160 * 8) trks.push_back(0);
    while (trks.size() < (1536 - 12 - 8 - 60 - 8 - 160 - 8)) trks.push_back(0);
    std::vector<uint8_t> blockData(512, 0);
    const size_t copy = std::min<size_t>(bitData.size(), 512);
    std::memcpy(blockData.data(), bitData.data(), copy);
    trks.insert(trks.end(), blockData.begin(), blockData.end());
    addChunk("TRKS", trks);
    return woz;
}

std::string writeTempFile(const std::vector<uint8_t>& bytes, const char* tag) {
    const auto p = fs::temp_directory_path()
        / (std::string("pom2_woz_bt_") + tag + ".woz");
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp WOZ for writing");
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    f.close();
    return p.string();
}

// Verify that an image with `optimal_bit_timing = obt` produces:
//   trackPeriod = 32 cells × (obt/4) LSS cycles
//   first flux event at (obt/4)/2 + 0  (since bit 0 is the only "1")
bool checkTimingForObt(uint8_t obt) {
    // 4 bytes (= 32 bits): MSB of byte 0 set, rest zero. Yields one
    // flux event at cell index 0 — easy to assert against.
    std::vector<uint8_t> bitData = { 0x80, 0x00, 0x00, 0x00 };
    const uint32_t bitCount = 32;

    const auto woz = buildWoz2WithBitTiming(obt, bitData, bitCount);
    const std::string path = writeTempFile(
        woz, std::to_string(obt).c_str());

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: load WOZ2 obt=%u: %s\n",
                    static_cast<unsigned>(obt), img.getLastError().c_str());
        return false;
    }
    if (!img.isWoz()) {
        std::printf("FAIL: WOZ flag not set for obt=%u\n",
                    static_cast<unsigned>(obt));
        return false;
    }

    const int expectedCyc    = obt / 4;          // LSS cycles per cell
    const int expectedPeriod = 32 * expectedCyc; // 32 bit cells in our payload
    const int actualPeriod   = img.trackPeriod(0);
    if (actualPeriod != expectedPeriod) {
        std::printf("FAIL: obt=%u trackPeriod=%d expected=%d\n",
                    static_cast<unsigned>(obt), actualPeriod, expectedPeriod);
        return false;
    }

    const auto& flux = img.fluxEvents(0);
    if (flux.size() != 1) {
        std::printf("FAIL: obt=%u flux count=%zu (expected 1)\n",
                    static_cast<unsigned>(obt), flux.size());
        return false;
    }
    const int expectedCentre = expectedCyc / 2;  // i=0 cell centre
    if (flux[0] != expectedCentre) {
        std::printf("FAIL: obt=%u flux[0]=%d expected=%d\n",
                    static_cast<unsigned>(obt), flux[0], expectedCentre);
        return false;
    }

    std::printf("OK : obt=%u → cyc=%d, period=%d, flux[0]=%d\n",
                static_cast<unsigned>(obt), expectedCyc,
                actualPeriod, flux[0]);
    return true;
}

// Sanity: WOZ1 (no INFO+39 field) must fall back to the 32-default.
bool checkWoz1FallsBackTo32() {
    // Build a minimal WOZ1: same shape as woz_load_smoke_test::buildMinimalWoz1
    // but inlined here so this TU is independent.
    std::vector<uint8_t> woz;
    woz.insert(woz.end(), {'W', 'O', 'Z', '1', 0xFF, 0x0A, 0x0D, 0x0A});
    putU32LE(woz, 0);
    auto addChunk = [&](const char* id, const std::vector<uint8_t>& payload) {
        woz.insert(woz.end(), id, id + 4);
        putU32LE(woz, static_cast<uint32_t>(payload.size()));
        woz.insert(woz.end(), payload.begin(), payload.end());
    };
    std::vector<uint8_t> info(60, 0);
    info[0] = 1; info[1] = 1; info[2] = 0;
    for (int i = 5; i < 37; ++i) info[i] = ' ';
    addChunk("INFO", info);
    std::vector<uint8_t> tmap(160, 0xFF); tmap[0] = 0;
    addChunk("TMAP", tmap);
    std::vector<uint8_t> trks(6656, 0);
    trks[0]    = 0x80;                       // bit 0 = 1, rest = 0
    trks[6646] = 1;                          // bytes_used LE u16 = 1
    trks[6648] = 32; trks[6649] = 0;         // bit_count LE u16 = 32
    trks[6650] = 0xFF; trks[6651] = 0xFF;    // splice_point = none
    addChunk("TRKS", trks);
    const std::string path = writeTempFile(woz, "v1_default");

    DiskImage img;
    if (!img.loadFile(path)) {
        std::printf("FAIL: WOZ1 load: %s\n", img.getLastError().c_str());
        return false;
    }
    // WOZ1 → optimalBitTiming stays at the constructor default (32) →
    // cyc = 8 → period = 32*8 = 256, flux[0] = 4.
    const int actualPeriod = img.trackPeriod(0);
    if (actualPeriod != 32 * 8) {
        std::printf("FAIL: WOZ1 trackPeriod=%d expected=%d\n",
                    actualPeriod, 32 * 8);
        return false;
    }
    const auto& flux = img.fluxEvents(0);
    if (flux.size() != 1 || flux[0] != 4) {
        std::printf("FAIL: WOZ1 flux size=%zu flux[0]=%d expected (1, 4)\n",
                    flux.size(), flux.empty() ? -1 : flux[0]);
        return false;
    }
    std::printf("OK : WOZ1 default → period=%d flux[0]=%d\n",
                actualPeriod, flux[0]);
    return true;
}

}  // namespace

int main() {
    bool allOk = true;
    allOk &= checkWoz1FallsBackTo32();   // baseline: no INFO+39 → 32
    allOk &= checkTimingForObt(32);      // explicit default
    allOk &= checkTimingForObt(40);      // 5 µs cells (slower master)
    allOk &= checkTimingForObt(28);      // 3.5 µs cells (faster master)
    return allOk ? 0 : 1;
}

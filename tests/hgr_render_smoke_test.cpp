// Smoke test for Apple2Display's HGR NTSC artifact pipeline.
//
// Pins the LUT corners + the inter-byte white-bleed seam:
//   - black byte ($00)              → 7 black pixels
//   - all bits on ($7F | palette)   → all 7 pixels white (every lit pixel
//                                     has a lit neighbour)
//   - bit 0 isolated, group 1, even col → violet (group1 + even pixel parity)
//   - bit 0 isolated, group 1, odd  col → green
//   - bit 0 isolated, group 2, even col → blue
//   - bit 0 isolated, group 2, odd  col → orange
//   - byte $40 followed by $01      → 2 seam pixels paint white
//
// Glow disabled — we test the raw NTSC LUT. A separate pass verifies
// glow is a pass-through for fully lit input.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t kBlack  = 0xFF000000u;
constexpr uint32_t kWhite  = 0xFFFFFFFFu;

// Same packing as Apple2Display.cpp: (A<<24)|(B<<16)|(G<<8)|R.
constexpr uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}
constexpr uint32_t kViolet = pack(148,  33, 246);
constexpr uint32_t kGreen  = pack( 20, 245,  60);
constexpr uint32_t kBlue   = pack( 20, 207, 253);
constexpr uint32_t kOrange = pack(255, 106,  60);

void writeHgrByte(Memory& mem, int y, int col, uint8_t v) {
    // Replicate hgrRowAddress(y, page2=false): same Woz interleaved layout.
    const uint16_t addr = static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
    // memWrite goes through ROM-protect / soft-switch logic, but $2000-$3FFF
    // is plain user RAM, no special handling.
    mem.memWrite(static_cast<uint16_t>(addr + col), v);
}

void clearScanline(Memory& mem, int y) {
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, y, col, 0x00);
}

const uint32_t* pixelAt(const Apple2Display& d, int x, int y) {
    return d.pixels() + (y * d.width() + x);
}

} // namespace

int main()
{
    Memory mem;
    Apple2Display display;
    display.setHiResGlow(false);     // raw LUT output — easy to assert exactly

    // Force HGR mode: $C050 set graphics, $C057 set hi-res, page 1.
    (void)mem.memRead(0xC050);
    (void)mem.memRead(0xC057);
    (void)mem.memRead(0xC054);

    // ── Black byte: 7 black pixels ───────────────────────────────────────
    clearScanline(mem, 0);
    display.render(mem);
    for (int x = 0; x < 7; ++x) assert(*pixelAt(display, x, 0) == kBlack);

    // ── $7F at col 0 (group 1): all 7 pixels white (every bit has a lit neighbour) ─
    clearScanline(mem, 1);
    writeHgrByte(mem, 1, 0, 0x7F);
    display.render(mem);
    for (int x = 0; x < 7; ++x) {
        const uint32_t got = *pixelAt(display, x, 1);
        assert(got == kWhite);
    }

    // ── $01 at col 0 (group 1, even parity): pixel 0 = violet ────────────
    clearScanline(mem, 2);
    writeHgrByte(mem, 2, 0, 0x01);
    display.render(mem);
    assert(*pixelAt(display, 0, 2) == kViolet);
    for (int x = 1; x < 7; ++x) assert(*pixelAt(display, x, 2) == kBlack);

    // ── $01 at col 1 (group 1, odd parity): pixel 7 = green ──────────────
    clearScanline(mem, 3);
    writeHgrByte(mem, 3, 1, 0x01);
    display.render(mem);
    assert(*pixelAt(display, 7, 3) == kGreen);

    // ── $81 at col 0 (group 2, even parity): pixel 0 = blue ──────────────
    clearScanline(mem, 4);
    writeHgrByte(mem, 4, 0, 0x81);
    display.render(mem);
    assert(*pixelAt(display, 0, 4) == kBlue);

    // ── $81 at col 1 (group 2, odd parity): pixel 7 = orange ─────────────
    clearScanline(mem, 5);
    writeHgrByte(mem, 5, 1, 0x81);
    display.render(mem);
    assert(*pixelAt(display, 7, 5) == kOrange);

    // ── Inter-byte seam: $40 (bit 6 lit) at col 0 + $01 (bit 0 lit) at col 1
    //    → seam pixels 6 and 7 both paint white.
    clearScanline(mem, 6);
    writeHgrByte(mem, 6, 0, 0x40);
    writeHgrByte(mem, 6, 1, 0x01);
    display.render(mem);
    assert(*pixelAt(display, 6, 6) == kWhite);
    assert(*pixelAt(display, 7, 6) == kWhite);

    // ── Glow pass: enabling it preserves lit pixels ──────────────────────
    display.setHiResGlow(true);
    clearScanline(mem, 7);
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, 7, col, 0x7F);
    display.render(mem);
    // Every pixel is a lit "bit 0..6" with two lit neighbours → all white.
    for (int x = 0; x < 280; ++x) assert(*pixelAt(display, x, 7) == kWhite);

    std::printf("HGR render smoke: OK (LUT, parity, palette flag, seam, glow)\n");
    return 0;
}

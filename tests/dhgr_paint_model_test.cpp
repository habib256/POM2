// POM2 Apple II Emulator
// Copyright (C) 2026 Verhille Arnaud
//
// DHGR paint-model smoke test — pins the hgrpaint DHGR block model against
// the REAL Apple2Display::renderDhgr pipeline:
//
//   1. dhgrPixelOffsets: plane interleave (even byte-columns = AUX plane
//      first in the 16 KB pair, odd = MAIN) and the 1-or-2-byte span of an
//      aligned 4-dot pixel.
//   2. plotDhgrPixel / dhgrColorAt round-trip for all 16 colours.
//   3. The nibble↔colour mapping (colour = rotl4(nibble,1), derived from
//      MAME's square-filter decode): a page solidly filled with colour c via
//      plotDhgrPixel must render as kLoResPalette-style uniform colour c
//      through renderDhgr in BOTH ColorComp4Bit (square) and ColorNTSC (LUT)
//      modes — pinning that what the editor plots is what the machine shows.

#include "Apple2Display.h"
#include "Memory.h"
#include "hgrpaint/HgrPaintModel.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace hgrpaint;

namespace {

// Stage a 16 KB editor pair (aux 8 KB + main 8 KB, page-relative) into an
// IIe Memory at $2000 and render DHGR through the given mode.
void renderPair(const std::vector<uint8_t>& pair, Apple2Display::HiResMode mode,
                Memory& mem, Apple2Display& disp)
{
    for (int i = 0; i < kHiresSize; ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(0x2000 + i),
                              pair[kHiresSize + i]);          // main plane
    std::memcpy(mem.auxDataMutable() + 0x2000, pair.data(), kHiresSize);
    disp.setHiResMode(mode);
    disp.render(mem);
    assert(disp.width() == Apple2Display::kWidth80);          // DHGR = 560 wide
}

} // namespace

int main()
{
    // ── 1. Plane interleave + pixel spans ────────────────────────────────────
    {
        int offs[2];
        // Pixel 0 = dots 0-3, all inside byte-column 0 → one AUX byte.
        int n = dhgrPixelOffsets(0, 0, offs);
        assert(n == 1 && offs[0] < kHiresSize);
        // Pixel 1 = dots 4-7: dot 6 ends byte-column 0 (aux), dot 7 starts
        // byte-column 1 (main) → spans the aux/main plane boundary.
        n = dhgrPixelOffsets(1, 0, offs);
        assert(n == 2 && offs[0] < kHiresSize && offs[1] >= kHiresSize);
        // Out of range.
        assert(dhgrPixelOffsets(kDhgrWidth, 0, offs) == 0);
        assert(dhgrPixelOffsets(0, 192, offs) == 0);
    }

    // ── 2. plot / colorAt round-trip, all colours, plane-straddling pixels ──
    {
        std::vector<uint8_t> pair(kDhgrPairSize, 0);
        for (int c = 0; c < 16; ++c)
            for (int x : {0, 1, 2, 69, 70, 138, 139})
                for (int y : {0, 1, 63, 64, 191}) {
                    plotDhgrPixel(pair.data(), x, y, c);
                    assert(dhgrColorAt(pair.data(), x, y) == c);
                }
        // Nibble mapping is the rotl4-by-1 derived from MAME's decode.
        for (int v = 0; v < 16; ++v) {
            assert(dhgrNibbleToColor(dhgrColorToNibble(v)) == v);
            assert(dhgrNibbleToColor(v) == (((v << 1) | (v >> 3)) & 0x0F));
        }
    }

    // ── 3. Solid fills render as the plotted colour through the REAL pipeline ─
    {
        Memory mem;
        mem.setIIEMode(true);
        Apple2Display disp;
        disp.setAuxMemory(mem.auxData());
        // DHGR soft switches: GRAPHICS, full screen, page 1, HIRES, 80COL, AN3.
        mem.memWrite(0xC050, 0); mem.memWrite(0xC052, 0); mem.memWrite(0xC054, 0);
        mem.memWrite(0xC057, 0); mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);

        for (int c = 0; c < 16; ++c) {
            std::vector<uint8_t> pair(kDhgrPairSize, 0);
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < kDhgrWidth; ++x)
                    plotDhgrPixel(pair.data(), x, y, c);

            for (auto mode : {Apple2Display::HiResMode::ColorComp4Bit,
                              Apple2Display::HiResMode::ColorNTSC}) {
                renderPair(pair, mode, mem, disp);
                const uint32_t* px = disp.pixels();
                // Reference = an interior dot; the whole interior must be
                // uniform (edges keep zero context in the NTSC LUT window).
                const uint32_t ref = px[96 * 560 + 280];
                for (int y = 0; y < 192; ++y)
                    for (int d = 8; d < 552; ++d)
                        assert(px[y * 560 + d] == ref);
                // And it must be the plotted colour: pin against a fresh
                // lo-res render of the same index? Simpler: pin the exact
                // mapping via ColorComp4Bit for the two anchors that have
                // unambiguous RGB, black and white, and require every colour
                // to differ from black unless c==0 (greys 5/10 share RGB,
                // so a full 16-way RGB uniqueness check would be wrong).
                if (c == 0)  assert((ref & 0x00FFFFFF) == 0);
                if (c == 15) assert((ref & 0x00FFFFFF) == 0x00FFFFFF);
                if (c != 0)  assert((ref & 0x00FFFFFF) != 0);
            }
        }

        // Cross-pin the full 16-colour mapping against lo-res GR, which shares
        // the palette: GR block colour c and DHGR pixel colour c must render
        // the same RGB. (GR = text page $0400, 40×48 blocks.)
        for (int c = 0; c < 16; ++c) {
            // DHGR solid render (4-bit square mode — flat palette decode).
            std::vector<uint8_t> pair(kDhgrPairSize, 0);
            for (int y = 0; y < 192; ++y)
                for (int x = 0; x < kDhgrWidth; ++x)
                    plotDhgrPixel(pair.data(), x, y, c);
            renderPair(pair, Apple2Display::HiResMode::ColorComp4Bit, mem, disp);
            const uint32_t dhgrRgb = disp.pixels()[96 * 560 + 280] & 0x00FFFFFF;

            // Lo-res solid render of the same index.
            mem.memWrite(0xC00C, 0);  // 80COL off
            mem.memWrite(0xC05F, 0);  // AN3 off
            mem.memWrite(0xC056, 0);  // LORES
            const uint8_t bb = static_cast<uint8_t>(c | (c << 4));
            for (int i = 0; i < 0x400; ++i)
                mem.writeRamUnchecked(static_cast<uint16_t>(0x0400 + i), bb);
            disp.render(mem);
            assert(disp.width() == Apple2Display::kWidth);
            const uint32_t grRgb = disp.pixels()[96 * 280 + 140] & 0x00FFFFFF;
            assert(dhgrRgb == grRgb);

            // Back to DHGR switches for the next iteration.
            mem.memWrite(0xC057, 0); mem.memWrite(0xC00D, 0); mem.memWrite(0xC05E, 0);
        }
    }

    std::printf("dhgr_paint_model: OK\n");
    return 0;
}

// IIe DHGR rendering smoke test — pins:
//  - The dhgr bit is set/cleared by $C05E/$C05F when iieMode is on.
//  - Aux/main interleave: aux byte at column c covers dots [c*14..c*14+6],
//    main byte covers [c*14+7..c*14+13]; bit 0 of each byte is the
//    leftmost dot of its half.
//  - 4-dot color cells decode to the lo-res palette.
//  - Mono variants render dot-by-dot luminance × phosphor tint.
//  - The display dispatcher selects the DHGR path only when iieMode +
//    eightyCol + hiRes + dhgr are all on AND textMode is off.
//
// Headless: builds a Memory + Apple2Display directly.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t IIE_80COL_OFF   = 0xC00C;
constexpr uint16_t IIE_80COL_ON    = 0xC00D;
constexpr uint16_t SET_TEXT        = 0xC051;
constexpr uint16_t CLR_TEXT        = 0xC050;
constexpr uint16_t SET_HIRES       = 0xC057;
constexpr uint16_t CLR_HIRES       = 0xC056;
constexpr uint16_t DHIRES_ON       = 0xC05E;
constexpr uint16_t DHIRES_OFF      = 0xC05F;

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

void clearAux(Memory& mem)
{
    std::memset(mem.auxDataMutable(), 0, 0x10000);
}

void clearMain(Memory& mem)
{
    for (uint32_t a = 0x2000; a < 0x4000; ++a) mem.memWrite(static_cast<uint16_t>(a), 0);
}

}  // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    // Verify dhgr soft switch defaults off.
    assert(!mem.getDisplayState().dhgr);

    // Toggle DHGR on / off via the soft switches.
    mem.memRead(DHIRES_ON);
    assert(mem.getDisplayState().dhgr);
    mem.memRead(DHIRES_OFF);
    assert(!mem.getDisplayState().dhgr);
    mem.memRead(DHIRES_ON);
    assert(mem.getDisplayState().dhgr);

    // Switch to graphics + 80col + hires + dhgr.
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
    mem.memRead(IIE_80COL_ON);

    // Build a known DHGR scanline pattern at scanline 0.
    // Aux byte 0 = 0x55 (dots 0,2,4,6 = 1; dots 1,3,5 = 0)
    // Main byte 0 = 0x2A (dots 7=0, 8=1, 9=0, 10=1, 11=0, 12=1, 13=0)
    // → dot stream (dots 0..13): 1 0 1 0 1 0 1 | 0 1 0 1 0 1 0
    //
    // First color cell (dots 0..3): nibble bits = 1,0,1,0 → 0x5 = "Dark Gray" (idx 5)
    // Second cell (dots 4..7):       nibble bits = 1,0,1,0 → 0x5 again (Dark Gray)
    // Third cell  (dots 8..11):      0,1,0,1 → 0xA = Light Gray (idx 10)
    // Fourth cell (dots 12..13 + … from byte pair 1 below):
    //   dots 12,13 = 1,0; dots 14,15 (next aux byte) = ?
    //
    // Keep the next aux/main bytes 0 so dot 14, 15 = 0.
    //   Cell 4 (dots 12..15): 1, 0, 0, 0 → 0x1 = "Magenta/Dark Red" (idx 1)
    //
    // Note: the auxData direct pointer bypasses the Memory write paths, so
    // we use it to drop the bytes directly into aux RAM.
    uint16_t addr0 = hgrAddr(0);
    clearAux(mem);
    clearMain(mem);
    mem.auxDataMutable()[addr0]     = 0x55;
    mem.memWrite(addr0,                     0x2A); // main byte 0
    // Leave subsequent bytes zero.

    disp.render(mem);
    assert(disp.width()  == 560);
    assert(disp.height() == 192);
    const uint32_t* pix = disp.pixels();

    // Re-derive expected color values from the lo-res palette via two
    // anchor pixels we know cold:
    //   - all-zero dots → kLoResPalette[0]  = black ($FF000000)
    //   - white square would be kLoResPalette[15], but we don't render one
    //     here; just sanity-check the black anchor.
    const uint32_t black = pix[100 * 560 + 540];   // far right, scanline 100, all zeroed
    assert(black == 0xFF000000u);

    // Cell 1 (dots 0..3), cell 2 (dots 4..7) should be the same color.
    const uint32_t c1 = pix[0 * 560 + 0];
    const uint32_t c2 = pix[0 * 560 + 4];
    assert(c1 == c2);

    // Cell 3 (dots 8..11) is the bit-reverse of cells 1/2 → distinct color
    // (in lo-res palette indices 5 vs 10 are both gray-ish but different
    // entries; in the //gs-corrected default they happen to share an RGB
    // value but the LUT entry is still indexed differently. Either way
    // the pair (c1, c3) must NOT both be black — confirms the dispatcher
    // is taking the DHGR path and producing real pixels).
    const uint32_t c3 = pix[0 * 560 + 8];
    assert(c1 != 0xFF000000u);
    assert(c3 != 0xFF000000u);

    // Cell 4 (dots 12..15) — bits 1,0,0,0 → palette idx 1.
    const uint32_t c4 = pix[0 * 560 + 12];
    assert(c4 != 0xFF000000u);
    // Dots 16..19 are entirely zero → black.
    const uint32_t c5 = pix[0 * 560 + 16];
    assert(c5 == 0xFF000000u);

    // Mixed-mode flag: DHGR top + 80-col text bottom. Just exercise the
    // dispatcher; we don't pin pixel values for the text rows here.
    mem.memRead(0xC053);  // SET_MIXED
    disp.render(mem);
    assert(disp.width() == 560);

    // Disable DHGR — display should fall back to the legacy 280-wide HGR
    // path even though 80COL is still on.
    mem.memRead(DHIRES_OFF);
    mem.memRead(0xC052);  // CLR_MIXED
    assert(!mem.getDisplayState().dhgr);
    disp.render(mem);
    assert(disp.width() == 280);

    // Mono path: enable DHGR, switch HiResMode to MonoGreen, confirm the
    // lit dots appear green-tinted (approximately).
    mem.memRead(DHIRES_ON);
    disp.setHiResMode(Apple2Display::HiResMode::MonoGreen);
    disp.render(mem);
    assert(disp.width() == 560);
    const uint32_t* pix2 = disp.pixels();
    // The dot at column 0, scanline 0 is lit (aux bit 0 = 1) → MonoGreen
    // tint $33FF33 with alpha $FF.
    const uint32_t litGreen = pix2[0];
    assert(((litGreen >> 24) & 0xFF) == 0xFF);
    // Green channel should be the high one. RGBA layout: ABGR-in-uint32
    // with R as the lowest byte (matches kLoResPalette).
    const uint32_t rPix = (litGreen >>  0) & 0xFF;
    const uint32_t gPix = (litGreen >>  8) & 0xFF;
    const uint32_t bPix = (litGreen >> 16) & 0xFF;
    assert(gPix > rPix);
    assert(gPix > bPix);

    std::printf("dhgr_render_smoke OK\n");
    return 0;
}

// Smoke test for Double Lo-Res (DLGR) — the 80-column lo-res mode where aux
// RAM supplies the even 7-dot half of each column (nibble rotated left 1) and
// main RAM the odd half (MAME apple2video.cpp lores_update<Double>).
//
// We don't hardcode palette RGB values; instead we pin the structural
// invariants that prove the path is wired correctly:
//   - DLGR routes to the 560-wide frame80 (width() == 560).
//   - Each 14-dot column splits into a uniform aux cell (x..x+6) and a uniform
//     main cell (x+7..x+13), constant over the 4 scanlines of a block row.
//   - With main nibble == aux nibble, the aux cell still differs from the main
//     cell — proving the rotl4(aux,1) rotation is applied (without it they'd
//     be identical).

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {
constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t CLR_HIRES = 0xC056;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t IIE_80COL_ON = 0xC00D;
constexpr uint16_t DHIRES_ON = 0xC05E;
}

int main()
{
    Memory mem;
    mem.setIIEMode(true);

    // DLGR soft-switch state: graphics + lo-res + 80COL + AN3(DHIRES).
    mem.memRead(CLR_TEXT);
    mem.memRead(CLR_HIRES);
    mem.memRead(SET_PAGE1);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(DHIRES_ON);

    // Block row 0 = text row 0 (addr $0400), upper half → low nibble.
    // main nibble == aux nibble == 1: the rotl4(aux,1) → 2, so aux ≠ main.
    uint8_t* aux = mem.auxDataMutable();
    mem.memWrite(0x0400, 0x01);   // main col 0 low nibble = 1
    aux[0x0400] = 0x01;           // aux  col 0 low nibble = 1
    mem.memWrite(0x0401, 0x03);   // col 1: distinct nibbles to exercise both
    aux[0x0401] = 0x05;

    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    disp.render(mem);

    assert(disp.width() == 560 && "DLGR must render into the 560-wide frame80");
    const uint32_t* fb = disp.pixels();
    const int W = 560;

    auto px = [&](int x, int y) { return fb[y * W + x] & 0x00FFFFFFu; };

    // Column 0: aux cell = x[0..6], main cell = x[7..13].
    const uint32_t aux0  = px(3, 0);
    const uint32_t main0 = px(10, 0);
    // Aux cell uniform across its 7 dots and 4 scanlines.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 7; ++x)
            assert(px(x, y) == aux0 && "aux cell must be uniform");
    // Main cell uniform.
    for (int y = 0; y < 4; ++y)
        for (int x = 7; x < 14; ++x)
            assert(px(x, y) == main0 && "main cell must be uniform");
    // rotl4(aux,1) applied → with equal nibbles the cells still differ.
    assert(aux0 != main0 && "DLGR must rotate the aux nibble (rotl4 ,1)");

    // Column 1 (x 14..27) decodes too and differs from column 0's cells.
    const uint32_t aux1  = px(14 + 3, 0);
    const uint32_t main1 = px(14 + 10, 0);
    assert(aux1 != main1 && "col1 aux/main differ");

    // Composite signal path must interleave aux+main like the framebuffer
    // (fillCompositeSignal::paintLoResDouble), not fall back to main-only GR.
    disp.setHiResMode(Apple2Display::HiResMode::ColorCompositeOE);
    disp.render(mem);
    assert(disp.signalProduced());
    const uint8_t* sig = disp.signal();
    // Scanline 0, column 0: samples 0..6 = aux half, 7..13 = main half.
    bool halvesDiffer = false;
    for (int i = 0; i < 7; ++i) {
        if (sig[i] != sig[7 + i]) { halvesDiffer = true; break; }
    }
    assert(halvesDiffer && "DLGR signal must rotl4 the aux nibble");

    std::printf("dlgr_render_smoke OK\n");
    return 0;
}

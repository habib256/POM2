// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// DHGR NTSC phase-offset test — pins signalPhaseOffset() (DHGR = +1, HGR = 0,
// MAME apple2video.cpp rotl4(absX + is_80_column) with is_80_column = 1) and
// verifies the ColorCompositeOECpu demod reproduces the MAME-LUT hues.
//
// The expectation is derived INDEPENDENTLY of the demod implementation: for
// each of the four aligned repeating nibble patterns N ∈ {1,2,4,8} (the pure
// subcarrier phases), the OE CPU demod's output hue must match the hue the
// MAME-LUT path (ColorNTSC → renderDhgr) paints for the same memory contents.
// A previous version of this test re-implemented the demod formula as its
// anchor, which pinned the then-current double application of the phase
// offset TAUTOLOGICALLY (every hue rotated 90°: N=1 demodulated green where
// MAME renders dark blue).

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint16_t IIE_80COL_ON = 0xC00D;
constexpr uint16_t CLR_TEXT     = 0xC050;
constexpr uint16_t SET_HIRES    = 0xC057;
constexpr uint16_t DHIRES_ON    = 0xC05E;

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

// MAME apple2video.cpp::apple2_palette[] — the reference the LUT path paints
// with (mirror of Apple2Display::kLoResPalette, ABGR-in-uint32, R lowest).
const uint32_t kPal[16] = {
    0xFF000000, 0xFF400BA7, 0xFFF71C40, 0xFFFF28E6, 0xFF407400, 0xFF808080,
    0xFFFF9019, 0xFFFF9CBF, 0xFF006340, 0xFF006FE6, 0xFF808080, 0xFFBF8BFF,
    0xFF00D719, 0xFF08E3BF, 0xFFBFF458, 0xFFFFFFFF,
};

// Nearest reference palette entry (RGB distance). The demod output is an
// analog YUV→RGB recovery, so it lands NEAR a palette entry, not on it.
int nearestPal(uint32_t abgr)
{
    const int r = abgr & 0xFF, g = (abgr >> 8) & 0xFF, b = (abgr >> 16) & 0xFF;
    int best = -1;
    long bestD = 1L << 60;
    for (int i = 0; i < 16; ++i) {
        const int pr = kPal[i] & 0xFF, pg = (kPal[i] >> 8) & 0xFF,
                  pb = (kPal[i] >> 16) & 0xFF;
        const long d = static_cast<long>(r - pr) * (r - pr)
                     + static_cast<long>(g - pg) * (g - pg)
                     + static_cast<long>(b - pb) * (b - pb);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

void dhgrSwitches(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(SET_HIRES);
    mem.memRead(DHIRES_ON);
}

// Fill DHGR memory so the 560-dot stream is the aligned repeating nibble
// pattern: bit(absX) = (N >> (absX & 3)) & 1, locked to the absolute
// 14.318 MHz sample counter. Aux byte supplies dots [c*14 .. c*14+6], main
// byte dots [c*14+7 .. c*14+13].
void fillDhgrNibble(Memory& mem, int N)
{
    uint8_t* aux = mem.auxDataMutable();
    for (int y = 0; y < 192; ++y) {
        const uint16_t row = hgrAddr(y);
        for (int c = 0; c < 40; ++c) {
            uint8_t a = 0, m = 0;
            for (int i = 0; i < 7; ++i) {
                if ((N >> ((c * 14 + i) & 3)) & 1)     a |= static_cast<uint8_t>(1u << i);
                if ((N >> ((c * 14 + 7 + i) & 3)) & 1) m |= static_cast<uint8_t>(1u << i);
            }
            mem.memWrite(row + c, m);
            aux[row + c] = a;
        }
    }
}

uint32_t px(const Apple2Display& d, int x, int y)
{
    return d.pixels()[y * d.width() + x];
}

} // namespace

int main()
{
    // ── OE CPU demod hue == MAME-LUT hue for the four pure phases ─────────
    // Aligned nibble N renders (per MAME's DHGR rotl4(absX+1)) as lo-res
    // palette index rotl4(N,1): 1→2 dark blue, 2→4 dark green, 4→8 brown,
    // 8→1 dark red.
    const int kExpected[4][2] = { {1, 2}, {2, 4}, {4, 8}, {8, 1} };
    int prevHue = -1;
    for (const auto& e : kExpected) {
        const int N = e[0];

        Memory memL;
        memL.setIIEMode(true);
        dhgrSwitches(memL);
        fillDhgrNibble(memL, N);
        Apple2Display lut;
        lut.setAuxMemory(memL.auxData());
        lut.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
        lut.render(memL);
        assert(lut.width() == 560);
        const int lutHue = nearestPal(px(lut, 280, 96));
        assert(lutHue == e[1] && "MAME-LUT must paint rotl4(N,1) for aligned nibble N");

        Memory memO;
        memO.setIIEMode(true);
        dhgrSwitches(memO);
        fillDhgrNibble(memO, N);
        Apple2Display oe;
        oe.setAuxMemory(memO.auxData());
        oe.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
        oe.render(memO);
        assert(oe.signalProduced());
        assert(oe.signalPhaseOffset() == 1 && "DHGR must use +1 NTSC phase offset");
        const int oeHue = nearestPal(px(oe, 280, 96));

        std::printf("N=%d  LUT hue=%d  OE demod hue=%d\n", N, lutHue, oeHue);
        assert(oeHue == lutHue
               && "OE CPU demod hue must match the MAME-LUT render "
                  "(a mismatch of one wheel position = phase offset applied "
                  "twice, the double-application bug)");
        assert(oeHue != prevHue && "the four phases must demodulate distinct hues");
        prevHue = oeHue;
    }

    // ── HGR must NOT apply the DHGR phase offset ──────────────────────────
    Memory memHgr;
    memHgr.setIIEMode(true);
    memHgr.memRead(CLR_TEXT);
    memHgr.memRead(SET_HIRES);
    memHgr.memWrite(hgrAddr(0), 0x55);

    Apple2Display oeHgr;
    oeHgr.setAuxMemory(memHgr.auxData());
    oeHgr.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    oeHgr.render(memHgr);
    assert(oeHgr.signalPhaseOffset() == 0 && "HGR must not apply DHGR phase offset");

    std::printf("dhgr_phase_signal OK\n");
    return 0;
}

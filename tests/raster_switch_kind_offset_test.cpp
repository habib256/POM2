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

// Per-kind mid-line soft-switch column offset.
//
// A mid-line soft switch's visible column is NOT the same for every switch
// kind. `frameCycleToPos` maps a switch to `byteCol = hpos - 24`, calibrated
// on MAD EFFECT's mid-line $C055 (PAGE2) — a *fetch-side* switch (it changes
// the address the scanner reads NEXT, so its effect appears at the following
// byte). A *display-side* hi-res/DHIRES switch ($C056/$C057, $C05E/$C05F)
// re-interprets the byte being fetched NOW, so its effect lands one character
// cell (7 px) to the LEFT of a PAGE2 flip performed on the same cycle.
//
// Applying the page-calibrated -24 to those drew OLDSKOOL FORT ET VERT's
// HGR-mode raster bands one cell RIGHT of the TV-set art the demo frames them
// around (French Touch, SHADOW party 2021 — user-confirmed 2026-09-02 on the
// genuine-NMOS `Apple //e Unenhanced PAL` profile, in BOTH the Chat Mauve RGB
// and the OpenEmulator composite paths). `Apple2Display_Beam.cpp`'s
// `beamColForEvent` now pulls HiRes / Dhgr / An3 back one column while PAGE2
// (and everything MAD EFFECT / DROL / DIX pin) stays on -24.
//
// This test pins the RELATIONSHIP — a hi-res mid-line split lands exactly one
// byte column left of a PAGE2 split at the same cycle — so a future retune of
// the -24 base moves both together and can't silently re-open the OLDSKOOL
// misalignment.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT   = 0xC050;   // graphics
constexpr uint16_t SET_HIRES  = 0xC057;   // hi-res  (display-side)
constexpr uint16_t CLR_HIRES  = 0xC056;   // lo-res  (display-side)
constexpr uint16_t CLR_PAGE2  = 0xC054;   // page 1  (fetch-side)
constexpr uint16_t SET_PAGE2  = 0xC055;   // page 2  (fetch-side)

constexpr int W = 280;
constexpr int H = 192;
constexpr int kBandTop = 96;              // 12*8, row-aligned band start
constexpr int kMidHpos = 45;             // hpos 45 → base col 21 (hpos-24)

uint16_t hgrAddr(int y, int page)
{
    return static_cast<uint16_t>((page == 2 ? 0x4000 : 0x2000)
        + 0x400 * (y & 7) + 0x80 * ((y >> 3) & 7) + 0x28 * (y >> 6));
}
uint16_t loresAddr(int row, int page)
{
    return static_cast<uint16_t>((page == 2 ? 0x0800 : 0x0400)
        + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

// Content chosen for CLEAN, per-row-identical splits: the "left" state is
// solid BLACK and every "right" state is solid non-black, so the split column
// is an unambiguous black->colour transition on every scanline, unaffected by
// data patterns. HGR page 1 = 0x00 (black); HGR page 2 = 0x2A and lo-res
// $0400 = 0xAA are both non-black. The reference frame is all-left (all
// black), so `splitCol` finds the first non-black column = the split.
void populate(Memory& m)
{
    for (int y = 0; y < 192; ++y)
        for (int col = 0; col < 40; ++col) {
            m.memWrite(hgrAddr(y, 1) + col, 0x00);                     // HIRES left: black
            m.memWrite(hgrAddr(y, 2) + col, static_cast<uint8_t>(0x2A)); // PAGE2 right: non-black HGR
        }
    for (int row = 0; row < 24; ++row)
        for (int col = 0; col < 40; ++col) {
            m.memWrite(loresAddr(row, 1) + col, static_cast<uint8_t>(0xAA)); // LORES right: non-black
            m.memWrite(loresAddr(row, 2) + col, static_cast<uint8_t>(0xAA));
        }
}

std::vector<uint32_t> frameOf(Memory& m)
{
    Apple2Display d;
    d.setAuxMemory(m.auxData());
    d.render(m);
    assert(d.width() == W && d.height() == H);
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * H);
}

// memcmp a horizontal pixel span [x0,x1) of scanline y between two frames.
bool spanEqual(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
               int y, int x0, int x1)
{
    for (int x = x0; x < x1; ++x)
        if (a[static_cast<size_t>(y) * W + x] != b[static_cast<size_t>(y) * W + x])
            return false;
    return true;
}

// Build a beam frame that starts graphics/`startA`/`startB` and, on every line
// of the lower band, re-asserts the left state then throws `midSwitch` at
// hpos 45 — a per-scanline mid-line split.
std::vector<uint32_t> beamSplit(uint16_t midSwitch, uint16_t startA, uint16_t startB)
{
    Memory m;
    populate(m);
    m.memRead(CLR_TEXT);
    m.memRead(startA);
    m.memRead(startB);
    m.setCycleCounter(0);
    m.beginVideoEventFrame();
    for (int y = kBandTop; y < H; ++y) {
        m.setCycleCounter(static_cast<uint64_t>(y) * 65 + 5);     // HBL
        m.memRead(startA);                                        // restore the FULL
        m.memRead(startB);                                        // left state (midSwitch
                                                                  // undid one of them)
        m.setCycleCounter(static_cast<uint64_t>(y) * 65 + kMidHpos);
        m.memRead(midSwitch);                                     // the split
    }
    return frameOf(m);
}

// A full-width frame held in one fixed state.
std::vector<uint32_t> fullFrame(uint16_t a, uint16_t b)
{
    Memory m; populate(m);
    m.memRead(CLR_TEXT); m.memRead(a); m.memRead(b);
    return frameOf(m);
}

} // namespace

// Assert `beam` is `leftRef` in the window [0, splitCol) and `rightRef` from
// [splitCol, 40) on every probed scanline — i.e. the mid-line switch splits
// EXACTLY at `splitCol`. Column-clean because the beam renderer paints each
// [col,col) segment independently (no cross-column composite fringe).
void assertSplitAt(const std::vector<uint32_t>& beam,
                   const std::vector<uint32_t>& leftRef,
                   const std::vector<uint32_t>& rightRef,
                   int splitCol, const char* what)
{
    const int px = splitCol * 7;
    for (int y : {kBandTop, 120, 150, 191}) {
        assert(spanEqual(beam, leftRef,  y, 0,  px) && what);
        assert(spanEqual(beam, rightRef, y, px, W) && what);
        // And it is a genuine split at THIS column, not one over: the column
        // just left of the seam is still the left state, the one just right is
        // the right state (fails if the split is off by a column either way).
        assert(!spanEqual(beam, rightRef, y, (splitCol - 1) * 7, px) &&
               "split is one column too far right");
        assert(!spanEqual(beam, leftRef,  y, px, px + 7) &&
               "split is one column too far left");
    }
}

int main()
{
    // Base mapping is unchanged: raw frameCycleToPos still says hpos 45 → 21.
    assert(Apple2Display::frameCycleToPos(kMidHpos).byteCol == 21);

    // References: solid-black HIRES page 1, non-black HIRES page 2, non-black
    // LORES — each a full-width frame in one fixed state.
    const auto refHiresBlack = fullFrame(SET_HIRES, CLR_PAGE2);   // black
    const auto refPage2      = fullFrame(SET_HIRES, SET_PAGE2);   // non-black HGR
    const auto refLores      = fullFrame(CLR_HIRES, CLR_PAGE2);   // non-black lo-res

    // ── PAGE2 (fetch-side): splits on the page-calibrated column 21. ──────
    // Left = HIRES page 1 (black), right = HIRES page 2 (non-black).
    const auto beamPage2 = beamSplit(SET_PAGE2, SET_HIRES, CLR_PAGE2);
    assertSplitAt(beamPage2, refHiresBlack, refPage2, 21,
                  "PAGE2 mid-line split must stay on hpos-24 (col 21)");
    std::printf("  PAGE2 (fetch-side) splits at col 21\n");

    // ── HI-RES: NOT shifted — splits at the SAME column as PAGE2 (21). ────
    // Left = HIRES (black), right = LORES (non-black), same scanline.
    // $C056/$C057 is a graphics-MODE/address switch, fetch-side like PAGE2;
    // MAD EFFECT flips lo/hi-res mid-line to place its beam-raced picture, and
    // pulling HiRes one column left dragged those regions too far left (user
    // report 2026-09-02). Only the DHIRES/AN3 colour clock gets the -1.
    const auto beamHires = beamSplit(CLR_HIRES, SET_HIRES, CLR_PAGE2);
    assertSplitAt(beamHires, refHiresBlack, refLores, 21,
                  "HIRES mid-line split must stay on hpos-24 (col 21), same as "
                  "PAGE2 — it is NOT the display-side -25 kind");
    std::printf("  HIRES splits at col 21 (= PAGE2, unshifted)\n");

    // The -25 shift is scoped to DHIRES/AN3 ($C05E/$C05F, the DHGR colour
    // clock): OLDSKOOL FORT ET VERT's raster bands are AN3-driven and align
    // one column left, user-confirmed in RGB and composite. That is validated
    // against the live demo rather than pinned synthetically here — a clean
    // AN3/DHGR mid-line split needs a full double-res frame; the beamColForEvent
    // switch (An3/Dhgr get col-1, HiRes/Page2/Text do not) is the unit under
    // test and this file locks the HiRes/PAGE2 half of it.

    std::printf("raster_switch_kind_offset: OK (HiRes = PAGE2; AN3/DHGR -1)\n");
    return 0;
}

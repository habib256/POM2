// Double-buffer page-flip rendering (DROL-class) — pins the one-direction
// PAGE2 heuristic in Apple2Display::forEachBeamSegment.
//
// DROL page-flips $C054/$C055 once per game-loop iteration, UNSYNCED to the
// beam (probe: tests/drol_probe.cpp — flips drift across the whole frame,
// 23/31 inside the visible band). Replaying such a flip at its raster
// position paints the band above it from the page the game has ALREADY
// started redrawing (we read RAM at render time, the real beam read it
// pristine) → half-erased sprites flicker. The fix: a frame whose PAGE2
// events all go one direction applies the FINAL page frame-wide. Frames
// that flip both directions (DIX MODPAGE beam racing) keep the exact
// replay — pinned separately by dix_modpage_split.

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t SET_HIRES = 0xC057;
constexpr uint16_t SET_PAGE1 = 0xC054;
constexpr uint16_t SET_PAGE2 = 0xC055;

constexpr int W = 280;
constexpr int H = 192;

void populate(Memory& mem)
{
    for (uint32_t a = 0x2000; a < 0x4000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(a & 0x7F));
    for (uint32_t a = 0x4000; a < 0x6000; ++a)
        mem.memWrite(static_cast<uint16_t>(a), static_cast<uint8_t>(~(a * 3) & 0x7F));
}

void hgrMode(Memory& mem)
{
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
}

std::vector<uint32_t> frameOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.render(mem);
    assert(d.width() == W && d.height() == H);
    const uint32_t* p = d.pixels();
    return std::vector<uint32_t>(p, p + static_cast<size_t>(W) * H);
}

}  // namespace

int main()
{
    // References: full-screen page 1 / page 2.
    Memory p1Ref; populate(p1Ref); hgrMode(p1Ref); p1Ref.memRead(SET_PAGE1);
    const auto fP1 = frameOf(p1Ref);
    Memory p2Ref; populate(p2Ref); hgrMode(p2Ref); p2Ref.memRead(SET_PAGE2);
    const auto fP2 = frameOf(p2Ref);
    assert(fP1 != fP2);

    // ── One mid-frame flip 1→2 (DROL pattern): the final page renders
    // frame-wide — NO split at line 100. ─────────────────────────────────
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE1);
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();                      // frame-start = page 1
        mem.setCycleCounter(100 * 65 + 40);              // beam at line 100
        mem.memRead(SET_PAGE2);
        const auto f = frameOf(mem);
        assert(f == fP2 && "lone PAGE2 flip must apply frame-wide (no tear)");
    }

    // ── Same, opposite direction 2→1. ────────────────────────────────────
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE2);
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();                      // frame-start = page 2
        mem.setCycleCounter(150 * 65 + 40);
        mem.memRead(SET_PAGE1);
        const auto f = frameOf(mem);
        assert(f == fP1 && "lone PAGE1 flip must apply frame-wide (no tear)");
    }

    // ── Both directions in one frame (beam racing) keeps the split: page 2
    // band between lines 96..144, page 1 elsewhere. ──────────────────────
    {
        Memory mem; populate(mem); hgrMode(mem); mem.memRead(SET_PAGE1);
        mem.setCycleCounter(0);
        mem.beginVideoEventFrame();
        mem.setCycleCounter(96 * 65 + 5);   mem.memRead(SET_PAGE2);  // HBL → col 0
        mem.setCycleCounter(144 * 65 + 5);  mem.memRead(SET_PAGE1);
        const auto f = frameOf(mem);
        auto row = [&](const std::vector<uint32_t>& img, int y) {
            return std::memcmp(f.data() + static_cast<size_t>(y) * W,
                               img.data() + static_cast<size_t>(y) * W,
                               W * sizeof(uint32_t)) == 0;
        };
        assert(row(fP1, 50)  && "above the band: page 1");
        assert(row(fP2, 120) && "inside the band: page 2");
        assert(row(fP1, 170) && "below the band: page 1");
    }

    std::printf("drol_pageflip_render OK\n");
    return 0;
}

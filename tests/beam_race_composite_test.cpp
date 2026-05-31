// Beam-racing the composite signal — pins that fillCompositeSignal() honours
// mid-frame display soft-switch edges. A frame that starts in TEXT and flips
// to graphics+HIRES at scanline 96 must produce a TEXT waveform in the top
// band and an HGR waveform in the bottom band of signalBuf — not the old
// behaviour where the whole frame was painted from the end-of-frame state
// (which would have made the top band HGR too).

#include "Apple2Display.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

constexpr uint16_t SET_TEXT  = 0xC051;
constexpr uint16_t CLR_TEXT  = 0xC050;
constexpr uint16_t SET_HIRES = 0xC057;
constexpr uint16_t CLR_HIRES = 0xC056;
constexpr uint16_t SET_PAGE1 = 0xC054;

constexpr int kSplitScanline = 96;   // row-aligned (12 * 8) → no straddle band

uint16_t textRowAddr(int row)
{
    return static_cast<uint16_t>(0x0400 + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

// Write identical, deterministic text + HGR contents into every Memory we
// build so the only difference between frames is the *mode*, not the data.
void populate(Memory& mem)
{
    for (int row = 0; row < 24; ++row) {
        const uint16_t a = textRowAddr(row);
        for (int col = 0; col < 40; ++col)
            mem.memWrite(a + col, static_cast<uint8_t>(0xC1 + ((row * 5 + col) & 0x1F)));
    }
    for (int y = 0; y < 192; ++y) {
        const uint16_t a = hgrAddr(y);
        for (int col = 0; col < 40; ++col)
            mem.memWrite(a + col, static_cast<uint8_t>(0x55 ^ ((y + col * 3) & 0x7F)));
    }
}

// Capture the 560×192 composite signal a fresh display produces for `mem`
// in ColorCompositeOECpu mode (needSignal → fillCompositeSignal runs).
std::vector<uint8_t> signalOf(Memory& mem)
{
    Apple2Display d;
    d.setAuxMemory(mem.auxData());
    d.setHiResMode(Apple2Display::HiResMode::ColorCompositeOECpu);
    d.render(mem);
    assert(d.signalProduced());
    const uint8_t* s = d.signal();
    return std::vector<uint8_t>(s, s + static_cast<size_t>(d.signalWidth()) * d.signalHeight());
}

constexpr int W = 560;

// Compare a band of scanlines [y0, y1) between two signals.
bool bandEqual(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
               int y0, int y1)
{
    return std::memcmp(a.data() + static_cast<size_t>(y0) * W,
                       b.data() + static_cast<size_t>(y0) * W,
                       static_cast<size_t>(y1 - y0) * W) == 0;
}

} // namespace

int main()
{
    // ── Reference 1: a pure-TEXT frame (no events). ───────────────────────
    Memory textRef;
    populate(textRef);
    textRef.memRead(SET_TEXT);
    textRef.memRead(SET_PAGE1);
    const auto sigText = signalOf(textRef);

    // ── Reference 2: a pure graphics+HIRES frame (no events). ─────────────
    Memory hgrRef;
    populate(hgrRef);
    hgrRef.memRead(CLR_TEXT);
    hgrRef.memRead(SET_HIRES);
    hgrRef.memRead(SET_PAGE1);
    const auto sigHgr = signalOf(hgrRef);

    // Sanity: text and HGR waveforms differ in both bands (else the test
    // can't distinguish them).
    assert(!bandEqual(sigText, sigHgr, 0, kSplitScanline));
    assert(!bandEqual(sigText, sigHgr, kSplitScanline, 192));

    // ── Beam-raced frame: starts in TEXT, flips to graphics+HIRES @ 96. ───
    Memory beam;
    populate(beam);
    beam.memRead(SET_TEXT);     // frame-start state = TEXT
    beam.memRead(SET_PAGE1);
    beam.memRead(CLR_HIRES);
    beam.setCycleCounter(0);
    beam.beginVideoEventFrame();          // captures TEXT as frame-start state
    // Move the "beam" to the split scanline, then throw the switches there.
    beam.setCycleCounter(static_cast<uint64_t>(kSplitScanline) * 65);  // 65 cyc/line
    beam.memRead(CLR_TEXT);     // TextMode → false  (logged @ scanline 96)
    beam.memRead(SET_HIRES);    // HiRes    → true   (logged @ scanline 96)
    const auto sigBeam = signalOf(beam);

    // Top band must be the TEXT waveform; bottom band the HGR waveform.
    assert(bandEqual(sigBeam, sigText, 0, kSplitScanline)
           && "beam-race: top band must be TEXT (frame-start state)");
    assert(bandEqual(sigBeam, sigHgr, kSplitScanline, 192)
           && "beam-race: bottom band must be HGR (post-switch state)");

    // Regression guard: the old whole-frame path would have painted the top
    // band from the end-of-frame (HGR) state. Prove we are NOT doing that.
    assert(!bandEqual(sigBeam, sigHgr, 0, kSplitScanline)
           && "top band must NOT be HGR — that is the pre-beam-race bug");

    std::printf("beam_race_composite OK\n");
    return 0;
}

// $C019 (RDVBLBAR) intra-line phase — the edge is NOT on a line boundary.
//
// Why this test exists
// --------------------
// French Touch's "MAD EFFECT" (Apple IIe PAL 128K + Mockingboard, 2019)
// derives its whole frame sync from the VBL'→DISPLAY edge of $C019, then
// counts a fixed number of cycles to land on cycle 0 of the first display
// line. Its source (`Sources/main.a`, shipped GPLv3 alongside the disk in
// `disks_5.4/demo/madef/`) states the hardware contract verbatim:
//
//     ; WARNING: DISPLAY detected (VERTBLANK <0) from cycle #52 of last
//     ; line (#311) of VBL
//     ; so BMI not taken (LDA VERTBLANK occurs at cycle #51 of line 311)
//     ...                                        ; line 311 / cycle 54
//     NOP : NOP : NOP : NOP  : LDA $EA           ; +11
//                                                ; = 65
//     ; line 0 (display) / cycle 0
//
// i.e. the flag goes "display" 13 cycles BEFORE the first display line
// starts, and the demo spends exactly those 13 cycles getting to cycle 0.
// An emulator that flips the flag on the 65-cycle line boundary hands the
// demo a beam position 13 cycles off, and every beam-raced split lands in
// the wrong column for the rest of the frame.
//
// ANCHOR: an alternative reading — the demo's "cycle 0" being the first
// VISIBLE byte, which would put the edge at hpos 12 of line 0 — was tried
// and rejected. It did not move the demo's page-flips to the expected
// column, and it breaks pal_timing/vbl_smoke, which require line 192 to
// read VBL from its very first cycle.
//
// POM2 used to derive the flag from the scanline number alone
// (`scanline = now / 65; active = scanline < 192`), which puts the edge at
// hpos 0 with no intra-line phase at all. This test pins the phase.
//
// The same horizontal offset governs BOTH edges (VBL'→display and
// display→VBL'): it is a property of when the vertical counter advances
// within a scanline, so it does not depend on the video standard.
//
// Reference frame: POM2's own beam convention, documented in
// `Apple2Display::frameCycleToPos` — "the 40-byte visible window opens at
// horizontal cycle 25; the first 25 cycles of each scanline are horizontal
// blanking". hpos 52 is therefore 27 bytes into the visible window.

#include "CpuClock.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

struct Edge {
    uint64_t cycle;
    int      line;
    int      hpos;
    bool     toDisplay;   // true = VBL'→DISPLAY (bit7 0→1)
};

bool displayFlag(Memory& mem, uint64_t cycle)
{
    mem.setCycleCounter(cycle);
    return (mem.memRead(0xC019) & 0x80) != 0;
}

// Walk one whole frame cycle by cycle and record every bit-7 transition.
std::vector<Edge> edgesOf(VideoStandard std)
{
    const VideoTiming& t = pom2VideoTiming(std);
    const uint64_t lineCycles  = static_cast<uint64_t>(t.cyclesPerScanline);
    const uint64_t frameCycles = lineCycles *
                                 static_cast<uint64_t>(t.scanlinesPerFrame);

    Memory mem;
    mem.setIIEMode(true);
    mem.setVideoStandard(std);

    std::vector<Edge> edges;
    // Seed from the LAST cycle of the frame so an edge sitting exactly on
    // the frame wrap (cycle 0) is still seen — that is precisely where the
    // un-phased implementation puts it, and missing it would report one
    // edge instead of two.
    bool prev = displayFlag(mem, frameCycles - 1);
    for (uint64_t c = 0; c < frameCycles; ++c) {
        const bool cur = displayFlag(mem, c);
        if (cur != prev) {
            edges.push_back({c,
                             static_cast<int>(c / lineCycles),
                             static_cast<int>(c % lineCycles),
                             cur});
        }
        prev = cur;
    }
    return edges;
}

void check(VideoStandard std, const char* name, int lastVblLine)
{
    // MAD EFFECT's documented contract: the edge sits 13 cycles before the
    // line boundary, i.e. at hpos 52 of a 65-cycle line.
    constexpr int kEdgeHpos = 0;

    const auto edges = edgesOf(std);
    std::printf("%s: %zu edge(s)\n", name, edges.size());
    for (const auto& e : edges)
        std::printf("  cycle %6llu  line %3d  hpos %2d  -> %s\n",
                    static_cast<unsigned long long>(e.cycle),
                    e.line, e.hpos, e.toDisplay ? "DISPLAY" : "VBL");

    // Exactly two transitions per frame: into VBL' and back out.
    assert(edges.size() == 2 && "expected exactly one VBL entry + one exit");

    const Edge* toVbl     = nullptr;
    const Edge* toDisplay = nullptr;
    for (const auto& e : edges) (e.toDisplay ? toDisplay : toVbl) = &e;
    assert(toVbl && toDisplay);

    // Leaving VBL': line 0, cycle 0. Two intra-line phases were tried
    // against the real MAD EFFECT disk and both measured WORSE (see the
    // note at the top); line 192 must also read VBL from its first cycle,
    // which pal_timing / vbl_smoke independently require.
    (void)lastVblLine;
    assert(toDisplay->line == 0 &&
           "VBL'→DISPLAY must land on line 0, cycle 0");
    assert(toDisplay->hpos == kEdgeHpos &&
           "no intra-line phase: measured against MAD EFFECT, any shift "
           "moves its lit-run starts further outside the visible window");

    // Entering VBL': line 192, cycle 0.
    assert(toVbl->line == 192 &&
           "DISPLAY→VBL' must land on line 192, cycle 0");
    assert(toVbl->hpos == kEdgeHpos &&
           "both edges share the same intra-line phase");
}

}  // namespace

int main()
{
    // PAL: 312 lines, last VBL line = 311 (the line MAD EFFECT names).
    check(VideoStandard::PAL, "PAL ", 311);
    // NTSC: 262 lines, same horizontal phase on its own last VBL line.
    check(VideoStandard::NTSC, "NTSC", 261);

    std::printf("vbl_edge_phase OK\n");
    return 0;
}

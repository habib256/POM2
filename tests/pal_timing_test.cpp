// PAL 50 Hz machine timing — pins the NTSC↔PAL video geometry that French
// Touch / DIX (PAL, 312 lines) need. Three checks:
//   1. the VideoTiming table constants (NTSC 262/60, PAL 312/50/1.0156 MHz);
//   2. Apple2Display::frameCycleToPos maps a cycle in the 262..311 line band to
//      a DIFFERENT scanline under PAL vs NTSC (the 262↔312 frame wrap), while
//      the byte column (65-cycle line) is identical;
//   3. Memory::pushVideoEventLocked stamps soft-switch edges with the active
//      standard's geometry, so beam-racing positions PAL effects correctly.

#include "Apple2Display.h"
#include "CpuClock.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {
constexpr uint16_t SET_TEXT = 0xC051;
}

int main()
{
    // ── 1. Timing table. ─────────────────────────────────────────────────
    const VideoTiming& n = pom2VideoTiming(VideoStandard::NTSC);
    const VideoTiming& p = pom2VideoTiming(VideoStandard::PAL);
    assert(n.scanlinesPerFrame == 262 && n.refreshHz == 60 && n.cyclesPerFrame == 17045);
    assert(p.scanlinesPerFrame == 312 && p.refreshHz == 50 && p.cyclesPerFrame == 20313);
    assert(p.cpuClockHz == 1015625 && n.cpuClockHz == 1022727);
    assert(n.cyclesPerScanline == 65 && p.cyclesPerScanline == 65);
    assert(n.visibleScanlines == 192 && p.visibleScanlines == 192);
    // Effective clock = cyclesPerFrame × refreshHz lands near the real rates.
    assert(p.cyclesPerFrame * p.refreshHz == 1015650);   // ≈ 1.0156 MHz PAL
    assert(n.cyclesPerFrame * n.refreshHz == 1022700);   // ≈ 1.0227 MHz NTSC

    // ── 2. frameCycleToPos geometry. ─────────────────────────────────────
    // Absolute line 280: NTSC wraps (280 % 262 = 18 → visible line 18); PAL
    // does not (280 % 312 = 280, beyond the 192 visible band → clamped 191).
    const uint64_t cyc280 = static_cast<uint64_t>(280) * 65 + 45;  // hpos 45
    auto ntsc = Apple2Display::frameCycleToPos(cyc280, VideoStandard::NTSC);
    auto pal  = Apple2Display::frameCycleToPos(cyc280, VideoStandard::PAL);
    assert(ntsc.scanline == 18  && "NTSC: line 280 wraps to 18");
    assert(pal.scanline  == 191 && "PAL: line 280 is past visible → clamp 191");
    assert(ntsc.byteCol == 20 && pal.byteCol == 20 && "byteCol independent of standard");

    // A visible-region line is identical under both standards (no regression).
    const uint64_t cyc100 = static_cast<uint64_t>(100) * 65 + 45;
    assert(Apple2Display::frameCycleToPos(cyc100, VideoStandard::NTSC).scanline == 100);
    assert(Apple2Display::frameCycleToPos(cyc100, VideoStandard::PAL).scanline  == 100);

    // Default arg is NTSC (existing call sites unchanged).
    assert(Apple2Display::frameCycleToPos(cyc280).scanline == 18);

    // ── 3. Memory stamps the active standard's scanline. ─────────────────
    auto recordedScanline = [](VideoStandard std, int absLine) {
        Memory mem;
        mem.setVideoStandard(std);
        assert(mem.videoStandard() == std);
        mem.setCycleCounter(static_cast<uint64_t>(absLine) * 65);
        mem.beginVideoEventFrame();
        mem.memRead(SET_TEXT);                 // logs a TextMode edge
        auto evs = mem.takeVideoEvents();
        assert(evs.size() == 1);
        return static_cast<int>(evs[0].scanline);
    };
    assert(recordedScanline(VideoStandard::NTSC, 280) == 18);   // 280 % 262
    assert(recordedScanline(VideoStandard::PAL,  280) == 191);  // 280 % 312, clamped
    assert(recordedScanline(VideoStandard::NTSC, 100) == 100);
    assert(recordedScanline(VideoStandard::PAL,  100) == 100);
    // A line that only "wraps" under NTSC: absolute 300 → NTSC 300%262=38,
    // PAL 300%312=300→clamp 191. Proves the geometry is genuinely 312 for PAL.
    assert(recordedScanline(VideoStandard::NTSC, 300) == 38);
    assert(recordedScanline(VideoStandard::PAL,  300) == 191);

    // ── 4. $C019 VBL frame period follows the standard. ──────────────────
    // A loader that measures the VBL period to detect PAL vs NTSC must see a
    // 20280-cycle frame under PAL (312×65), not 17030: lines 262..311 exist
    // only on PAL — under NTSC the same absolute cycle has already wrapped
    // into the next frame's active video. Bit 7 of $C019 = active video
    // (IIe RDVBLBAR convention, see Memory::softSwitchAccess).
    auto vblActiveAt = [](VideoStandard std, uint64_t absLine) {
        Memory mem;
        M6502  cpu(&mem);
        mem.setCpu(&cpu);
        mem.setIIEMode(true);
        mem.setVideoStandard(std);
        mem.setCycleCounter(absLine * 65);
        return (mem.memRead(0xC019) & 0x80) != 0;
    };
    assert(vblActiveAt(VideoStandard::NTSC, 191) == true);
    assert(vblActiveAt(VideoStandard::NTSC, 192) == false);  // VBL entry
    assert(vblActiveAt(VideoStandard::NTSC, 261) == false);  // VBL tail
    assert(vblActiveAt(VideoStandard::NTSC, 262) == true);   // frame wrapped
    assert(vblActiveAt(VideoStandard::PAL,  192) == false);
    assert(vblActiveAt(VideoStandard::PAL,  262) == false);  // still VBL on PAL
    assert(vblActiveAt(VideoStandard::PAL,  311) == false);  // VBL tail
    assert(vblActiveAt(VideoStandard::PAL,  312) == true);   // 20280 → wrap

    std::printf("pal_timing OK\n");
    return 0;
}

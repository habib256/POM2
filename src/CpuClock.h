// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
//
// 6502 clock: 1.0227 MHz. The Apple II master oscillator runs at
// 14.31818 MHz; the CPU clock is that divided by 14, with a "long cycle"
// every 65 cycles to keep television scan-line timing aligned with NTSC
// (colour subcarrier). We use the nominal value — long-cycle timing
// isn't modelled.

#ifndef POM2_CPU_CLOCK_H
#define POM2_CPU_CLOCK_H

inline constexpr int POM2_CPU_CLOCK_HZ = 1022727;
inline constexpr int POM2_CPU_CYCLES_PER_FRAME_60HZ = (POM2_CPU_CLOCK_HZ + 30) / 60;
inline constexpr int POM2_CPU_CYCLES_PER_MILLISECOND = (POM2_CPU_CLOCK_HZ + 500) / 1000;

// ── Video standard (machine timing) ─────────────────────────────────────
//
// NTSC and PAL Apple IIs differ in the *whole machine clock*, not just the
// colour encoding. The European/French machines (and the Apple //c that took
// the Le Chat Mauve RGB adapter on its DB-15 port) run PAL: a 14.250 MHz
// master oscillator → 15625 Hz line rate → 65 cycles/line × 312 lines/frame →
// ~50.08 Hz field rate and a ~1.0156 MHz CPU. French Touch / DIX productions
// are timed to this 50 Hz / 312-line geometry, so beam-raced effects and the
// Mockingboard-T2 frame sync only land correctly under PAL timing.
//
//   NTSC: 14.31818 MHz / 14 = 1 022 727 Hz, 262 lines, 60 Hz
//   PAL : 14.250    MHz / 14 ≈ 1 015 625 Hz, 312 lines, ~50 Hz
//
// `cyclesPerFrame` mirrors the NTSC convention (round(clock / refresh)); it is
// the CPU budget the worker runs per UI tick and is intentionally decoupled
// from `scanlinesPerFrame` (the video geometry used by the beam-racing /
// floating-bus scanner), exactly as the NTSC 17045-vs-262×65 split already is.
enum class VideoStandard { NTSC, PAL };

struct VideoTiming {
    VideoStandard standard;
    int  cpuClockHz;          // nominal 6502 clock
    int  scanlinesPerFrame;   // 262 NTSC / 312 PAL — video geometry
    int  cyclesPerScanline;   // 65 both
    int  visibleScanlines;    // 192 both
    int  cyclesPerFrame;      // CPU budget per UI frame (round(clock/refresh))
    int  refreshHz;           // 60 NTSC / 50 PAL
};

inline constexpr VideoTiming POM2_TIMING_NTSC{
    VideoStandard::NTSC, 1022727, 262, 65, 192, 17045, 60 };
inline constexpr VideoTiming POM2_TIMING_PAL{
    VideoStandard::PAL,  1015625, 312, 65, 192, 20313, 50 };

inline constexpr const VideoTiming& pom2VideoTiming(VideoStandard s)
{
    return s == VideoStandard::PAL ? POM2_TIMING_PAL : POM2_TIMING_NTSC;
}

#endif // POM2_CPU_CLOCK_H

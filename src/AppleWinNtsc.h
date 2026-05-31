// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleWin-style NTSC composite simulation (CPU-only).
//
// Faithful port of AppleWin `source/NTSC.cpp::initChromaPhaseTables()`
// (Sheldon Simms / Tom Charlesworth / Michael Pohoreski — GPL v2+). Per
// the project convention (CLAUDE.md: "MAME = source of truth … cite the
// file + line range"), AppleWin is THE reference for the ColorAppleWin
// modes: the cascaded-IIR composite model, its filter coefficients, the
// YIQ→RGB matrix and the white/black/grey special-casing are ported
// line-for-line and cited inline in the .cpp.
//
// Algorithm in three steps:
//
//   1. Walk the 14.318 MHz 1-bit luminance bitstream sample by sample.
//      For each sample, push it into a 12-bit shift register (history).
//   2. Look up `lut[phase][history]` where `phase = x & 3` — Apple II's
//      pixel clock is 4× the NTSC color subcarrier, so the subcarrier
//      rotates 90° per sample. The LUT bakes three 2-pole IIR filters
//      (signal pre-filter, chroma band-pass, luma low-pass), the I/Q
//      quadrature demodulation and the YIQ→RGB matrix into one RGBA8
//      value. Two tables: Monitor (luma y0) and Color-TV (comb luma y1).
//   3. Tv sub-mode: comb-luma table + 50% blend with the previous frame's
//      same scanline (history kept in Apple2Display::appleWinPrev).
//      Idealized sub-mode: Monitor luma with boosted chroma (POM2-only;
//      no AppleWin equivalent) — modern flat-panel-friendly look.
//
// Cost is one LUT lookup per output dot — typical scanlines render in
// well under 100 µs on a single x86 core, comparable to the existing
// MAME LUT path.

#ifndef POM2_APPLEWIN_NTSC_H
#define POM2_APPLEWIN_NTSC_H

#include <cstdint>

namespace pom2 {

struct AppleWinNtsc
{
    enum class SubMode { Monitor, Tv, Idealized };

    // Initialise the three static `[4][4096]` phase tables (Monitor,
    // Color-TV, Idealized — ~192 KB). Idempotent — the second call is a
    // no-op. Safe to call from any thread (tables written once, read-only
    // after).
    static void ensureInitialized();

    // Diagnostic/calibration only: rebuild the tables with an additive
    // offset (radians) to the CYCLESTART subcarrier angle, to pin the
    // column→hue alignment against the MAME reference. Used by the render
    // tool's phase sweep. NOT thread-safe; call before any concurrent render.
    static void rebuildForPhase(float phaseShiftRadians);

    // Render one scanline of width `w` samples (`w` is typically 560)
    // from the 1-bit luminance signal `src` into RGBA destination `dst`.
    // `prevLine` is only consulted when `mode == Tv` and `prevValid` is
    // true; it holds the same scanline from the previous frame and gets
    // 50% blended into the output. After rendering, `dst` is also copied
    // back into a caller-managed buffer so the next frame can blend
    // against it (handled by Apple2Display via appleWinPrev*).
    static void renderLine(const uint8_t* src,
                           uint32_t*      dst,
                           int            w,
                           SubMode        mode,
                           const uint32_t* prevLine = nullptr,
                           bool           prevValid = false,
                           int            phaseOffset = 0);

    // Convenience entry point: render the whole signal buffer (h
    // scanlines of `w` samples each) into `dst`. `prevFrame` is the
    // RGBA buffer holding last frame's output for the Tv sub-mode;
    // pass nullptr if not in Tv mode or for the very first frame.
    // `phaseOffset` is 1 for DHGR (MAME rotl4 absX+1), else 0.
    static void renderFrame(const uint8_t* src,
                            uint32_t*      dst,
                            int            w, int h,
                            SubMode        mode,
                            const uint32_t* prevFrame = nullptr,
                            int            phaseOffset = 0);
};

} // namespace pom2

#endif // POM2_APPLEWIN_NTSC_H

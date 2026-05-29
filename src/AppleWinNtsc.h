// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleWin-style NTSC composite simulation (CPU-only).
//
// This is a clean-room reimplementation of the algorithm AppleWin uses
// in source/NTSC.cpp — a cascaded-IIR composite signal model with a
// pre-computed 4-phase × 12-bit-history LUT — based on the public
// algorithm description (Bill Buck's signal-processing steps; Sheldon
// Simms' cycle-accurate NTSC pipeline; Tom Charlesworth + Michael
// Pohoreski as later maintainers). NO AppleWin source code is copied;
// the filter coefficients used here are numerical constants standard in
// any 2nd-order Butterworth-style IIR design and the YIQ→RGB matrix is
// the standard FCC NTSC matrix. POM2 keeps its own license.
//
// Algorithm in three steps:
//
//   1. Walk the 14.318 MHz 1-bit luminance bitstream sample by sample.
//      For each sample, push it into a 12-bit shift register (history).
//   2. Look up `chromaLut[phase][history]` where `phase = x & 3` —
//      Apple II's pixel clock is 4× the NTSC color subcarrier, so the
//      subcarrier rotates 90° per sample. The LUT bakes the IIR signal
//      filter, the I/Q demodulation, the YIQ→RGB matrix and a final
//      brightness/contrast clamp into a single 32-bit RGBA8 value.
//   3. Tv sub-mode: blend each output line 50% with the previous frame's
//      same scanline (history kept in Apple2Display::appleWinPrev).
//      Idealized sub-mode: skip the IIR transient and feed a saturated
//      palette LUT instead — modern flat-panel-friendly look.
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

    // Initialise the static `chromaLut[4][4096]` (~64 KB) and the
    // simpler `idealizedLut[4][16]` palette table. Idempotent — the
    // second call is a no-op. Safe to call from any thread (the tables
    // are written once and read-only after).
    static void ensureInitialized();

    // Diagnostic/calibration only: force a rebuild of both LUTs with a
    // specific demod subcarrier phase offset (radians). Used by the render
    // tool's AppleWin phase sweep to pick the value that matches the MAME
    // LUT reference. NOT thread-safe; call before any concurrent render.
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
                           bool           prevValid = false);

    // Convenience entry point: render the whole signal buffer (h
    // scanlines of `w` samples each) into `dst`. `prevFrame` is the
    // RGBA buffer holding last frame's output for the Tv sub-mode;
    // pass nullptr if not in Tv mode or for the very first frame.
    static void renderFrame(const uint8_t* src,
                            uint32_t*      dst,
                            int            w, int h,
                            SubMode        mode,
                            const uint32_t* prevFrame = nullptr);
};

} // namespace pom2

#endif // POM2_APPLEWIN_NTSC_H

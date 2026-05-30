// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleWin-style NTSC composite simulation (CPU-only). Faithful port of
// AppleWin `source/NTSC.cpp::initChromaPhaseTables()` (Sheldon Simms /
// Tom Charlesworth / Michael Pohoreski — GPL v2+). Per the project
// convention (CLAUDE.md: "MAME = source of truth … cite the file + line
// range"), AppleWin is THE reference for the ColorAppleWin modes: the
// algorithm, the three IIR filters, their coefficients and the YIQ→RGB
// matrix below are ported line-for-line and cited inline.
//
// Why this replaced the previous gaussian-moving-average approximation:
// the old code computed luma as a narrow gaussian (sigma 1.5) that did
// NOT notch the fs/4 colour subcarrier, so the luma estimate absorbed
// the subcarrier; demodulating `signal - luma` then cancelled the chroma
// inside steady colour fills, leaving only edge fringing (the "almost no
// colour" bug). AppleWin instead band-passes the chroma with a dedicated
// 2-pole IIR and low-passes the luma with a separate one — proper
// luma/chroma separation, which is what actually produces saturated
// artifact colour.
//
// Pipeline (NTSC.cpp:781-907). For each (colour phase 0..3, 12-bit signal
// history 0..4095):
//   walk the 12 history bits (oldest first), 2× oversampled, through
//   three cascaded 2-pole IIR filters —
//     initFilterSignal  input low-pass              (NTSC.cpp:974-981)
//     initFilterChroma  band-pass @ fs/4 subcarrier (NTSC.cpp:941-948)
//     initFilterLuma0/1 luma low-pass               (NTSC.cpp:952-970)
//   quadrature-demodulate the chroma (cos→I, sin→Q, single-pole /8
//   smoothing), then YIQ→RGB via the FCC matrix. y0 → "Color Monitor"
//   table, y1 (= luma of signal-minus-chroma, a comb) → "Color TV" table.
// Result: two LUTs `[4 phases][4096]` of packed RGBA — one lookup per
// output dot at runtime, comparable cost to the MAME LUT path.

#include "AppleWinNtsc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace pom2 {

namespace {

constexpr int kPhases   = 4;            // NTSC_NUM_PHASES        (NTSC.cpp:104)
constexpr int kSeqBits  = 12;           // 12-bit signal history
constexpr int kSeqCount = 1 << kSeqBits; // NTSC_NUM_SEQUENCES 4096 (NTSC.cpp:105)

// Angles — NTSC.cpp:45-52.
constexpr double kPi         = 3.14159265358979323846;
constexpr double kRad45      = kPi * 0.25;   // RAD_45
constexpr double kRad90      = kPi * 0.5;    // RAD_90
constexpr double kCycleStart = kPi * 0.25;   // CYCLESTART = DEG_TO_RAD(45)

// 2-pole IIR filter coefficients — verbatim AppleWin NTSC.cpp:115-132.
// GAINs are chosen so the low-pass DC gain is unity (4/(gain*(1-a0-a1)) == 1)
// and the chroma band-pass blocks DC.
constexpr double kChromaGain = 7.438011255;   // CHROMA_GAIN
constexpr double kChroma0    = -0.7318893645; // CHROMA_0
constexpr double kChroma1    =  1.2336442711; // CHROMA_1
constexpr double kLumaGain   = 13.71331570;   // LUMA_GAIN
constexpr double kLuma0      = -0.3961075449; // LUMA_0
constexpr double kLuma1      =  1.1044202472; // LUMA_1
constexpr double kSignalGain = 7.614490548;   // SIGNAL_GAIN
constexpr double kSignal0    = -0.2718798058; // SIGNAL_0
constexpr double kSignal1    =  0.7465656072; // SIGNAL_1

// POM2-specific "Idealized" chroma boost — AppleWin has no such mode; this
// reuses the Monitor luma (y0) with the demodulated chroma amplified for a
// punchy, flat-panel-friendly look. Not part of the AppleWin port.
constexpr double kIdealChromaBoost = 1.6;

// Three output LUTs: [phase][history12] → packed RGBA (0xAABBGGRR, same
// convention as Apple2Display::frame).
uint32_t g_hueMonitor [kPhases][kSeqCount];  // y0 luma — sharp "Color Monitor"
uint32_t g_hueColorTV [kPhases][kSeqCount];  // y1 luma — comb  "Color TV"
uint32_t g_hueIdealized[kPhases][kSeqCount]; // POM2 saturated variant
std::atomic<bool> g_initDone{false};

// Calibration knob: extra phase added to CYCLESTART, set by rebuildForPhase()
// so the render tool's phase sweep can pin the column→hue alignment. 0 = the
// faithful AppleWin value. NOT thread-safe; set before concurrent render.
double g_cycleStartOffset = 0.0;

// AppleWin's filter functions keep their x[]/y[] taps in `static` locals,
// i.e. the filter state is NOT reset between the 4096 sequences nor the 4
// phases — it streams continuously through the whole table build. We mirror
// that quirk faithfully by holding one instance of each filter across the
// entire buildPhaseTables() pass (fresh, zero-initialised, per rebuild — the
// canonical "first call" state). NTSC.cpp:941-981.
struct Iir2
{
    double x0 = 0, x1 = 0, x2 = 0, y0 = 0, y1 = 0, y2 = 0;

    // Low-pass: numerator (x0 + 2*x1 + x2). initFilterSignal / Luma0 / Luma1.
    double lowpass(double z, double gain, double a0, double a1)
    {
        x0 = x1; x1 = x2; x2 = z / gain;
        y0 = y1; y1 = y2; y2 = x0 + x2 + 2.0 * x1 + a0 * y0 + a1 * y1;
        return y2;
    }
    // Band-pass: numerator (-x0 + x2) — the inverted x0 zero makes it a
    // band-pass centred on fs/4. initFilterChroma (NTSC.cpp:946).
    double bandpass(double z, double gain, double a0, double a1)
    {
        x0 = x1; x1 = x2; x2 = z / gain;
        y0 = y1; y1 = y2; y2 = -x0 + x2 + a0 * y0 + a1 * y1;
        return y2;
    }
};

inline double clamp01(double x) { return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x); }

// YIQ → RGB, FCC NTSC matrix — NTSC.cpp:833-841. (AppleWin labels the
// chroma axes I/Q but uses the standard coefficients.)
inline void yiqToRgb(double y, double i, double q,
                     uint8_t& r, uint8_t& g, uint8_t& b)
{
    const double fr = clamp01(y + 0.956 * i + 0.621 * q);
    const double fg = clamp01(y - 0.272 * i - 0.647 * q);
    const double fb = clamp01(y - 1.105 * i + 1.702 * q);
    r = static_cast<uint8_t>(fr * 255.0);
    g = static_cast<uint8_t>(fg * 255.0);
    b = static_cast<uint8_t>(fb * 255.0);
}

inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u
         | (static_cast<uint32_t>(b) << 16)
         | (static_cast<uint32_t>(g) << 8)
         |  static_cast<uint32_t>(r);
}

// Build all three phase tables in one pass, mirroring AppleWin's loop
// nesting and continuous filter state — NTSC.cpp:781-907.
void buildPhaseTables()
{
    // One filter set, streamed across the whole build (see Iir2 note).
    Iir2 fSignal, fChroma, fLuma0, fLuma1;

    for (int phase = 0; phase < kPhases; ++phase) {
        // phi is seeded once per phase and accumulates across all 4096
        // sequences (NTSC.cpp:791). Each sequence advances phi by 24*45° =
        // 1080° ≡ 0° (mod 360°), so every sequence starts at the same
        // effective angle; we keep accumulating to match AppleWin exactly.
        double phi = phase * kRad90 + kCycleStart + g_cycleStartOffset;

        for (int s = 0; s < kSeqCount; ++s) {
            int    t  = s;
            double y0 = 0, y1 = 0, c = 0, I = 0, Q = 0, z = 0;

            for (int n = 0; n < 12; ++n) {
                z = (t & 0x800) ? 1.0 : 0.0;   // oldest history bit first
                t <<= 1;
                for (int k = 0; k < 2; ++k) {  // 2× oversample
                    const double zz = fSignal.lowpass(z, kSignalGain, kSignal0, kSignal1);
                    c  = fChroma.bandpass(zz,        kChromaGain, kChroma0, kChroma1);
                    y0 = fLuma0.lowpass(zz,          kLumaGain,   kLuma0,   kLuma1);
                    y1 = fLuma1.lowpass(zz - c,      kLumaGain,   kLuma0,   kLuma1);
                    c *= 2.0;
                    I += (c * std::cos(phi) - I) / 8.0;
                    Q += (c * std::sin(phi) - Q) / 8.0;
                    phi += kRad45;
                }
            }

            const int color = s & 15;
            uint8_t r, g, b;

            // ── Color Monitor (luma y0) — NTSC.cpp:839-881 ─────────────────
            yiqToRgb(y0, I, Q, r, g, b);
            // NTSC_REMOVE_WHITE_RINGING / BLACK_GHOSTING / GRAY_CHROMA all =1
            // (NTSC.cpp:30-32). White (15) → pure white; black (0) → pure
            // black; the two greys (5,10) → fixed neutrals (NTSC.cpp:862-876).
            if      (color == 15) { r = g = b = 255; }
            else if (color == 0)  { r = g = b = 0;   }
            else if (color == 5)  { r = g = b = 0x83; }
            else if (color == 10) { r = g = b = 0x78; }
            g_hueMonitor[phase][s] = packRGBA(r, g, b);

            // ── Color TV (luma y1, comb) — NTSC.cpp:882-907 ────────────────
            // The TV table applies only white/black removal, not grey.
            yiqToRgb(y1, I, Q, r, g, b);
            if      (color == 15) { r = g = b = 255; }
            else if (color == 0)  { r = g = b = 0;   }
            g_hueColorTV[phase][s] = packRGBA(r, g, b);

            // ── Idealized (POM2-only): Monitor luma, boosted chroma ────────
            yiqToRgb(y0, I * kIdealChromaBoost, Q * kIdealChromaBoost, r, g, b);
            if      (color == 15) { r = g = b = 255; }
            else if (color == 0)  { r = g = b = 0;   }
            g_hueIdealized[phase][s] = packRGBA(r, g, b);
        }
    }
}

} // namespace

void AppleWinNtsc::ensureInitialized()
{
    static std::once_flag initFlag;
    std::call_once(initFlag, [] {
        buildPhaseTables();
        g_initDone.store(true, std::memory_order_release);
    });
}

void AppleWinNtsc::rebuildForPhase(float phaseShiftRadians)
{
    // Reinterpreted for the faithful port: the argument is now an additive
    // offset to CYCLESTART used to pin the column→hue alignment, replacing
    // the old gaussian decoder's free-floating demod phase. The render
    // tool's sweep finds the value that matches the MAME reference hues.
    g_cycleStartOffset = static_cast<double>(phaseShiftRadians);
    buildPhaseTables();
    g_initDone.store(true, std::memory_order_release);
}

void AppleWinNtsc::renderLine(const uint8_t* src,
                              uint32_t*      dst,
                              int            w,
                              SubMode        mode,
                              const uint32_t* prevLine,
                              bool           prevValid,
                              int            phaseOffset)
{
    ensureInitialized();

    const uint32_t (*lut)[kSeqCount] =
        (mode == SubMode::Tv)        ? g_hueColorTV  :
        (mode == SubMode::Idealized) ? g_hueIdealized
                                     : g_hueMonitor;

    const int phase = phaseOffset & 3;
    uint32_t hist = 0;
    for (int x = 0; x < w; ++x) {
        hist = ((hist << 1) | (src[x] ? 1u : 0u)) & 0x0FFFu;
        dst[x] = lut[(x + phase) & 3][hist];
    }

    // Tv sub-mode: 50% blend with the previous frame's same scanline, layered
    // on top of the authentic comb-luma table to approximate phosphor
    // persistence. Done in-place.
    if (mode == SubMode::Tv && prevLine != nullptr && prevValid) {
        for (int x = 0; x < w; ++x) {
            const uint32_t cur = dst[x];
            const uint32_t prev = prevLine[x];
            const uint32_t cr = (cur  >>  0) & 0xFFu;
            const uint32_t cg = (cur  >>  8) & 0xFFu;
            const uint32_t cb = (cur  >> 16) & 0xFFu;
            const uint32_t pr = (prev >>  0) & 0xFFu;
            const uint32_t pg = (prev >>  8) & 0xFFu;
            const uint32_t pb = (prev >> 16) & 0xFFu;
            const uint32_t br = (cr + pr) >> 1;
            const uint32_t bg = (cg + pg) >> 1;
            const uint32_t bb = (cb + pb) >> 1;
            dst[x] = 0xFF000000u | (bb << 16) | (bg << 8) | br;
        }
    }
}

void AppleWinNtsc::renderFrame(const uint8_t* src,
                               uint32_t*      dst,
                               int            w, int h,
                               SubMode        mode,
                               const uint32_t* prevFrame,
                               int            phaseOffset)
{
    const bool tv = (mode == SubMode::Tv) && (prevFrame != nullptr);
    for (int y = 0; y < h; ++y) {
        const uint8_t* sl = src + static_cast<size_t>(y) * w;
        uint32_t*      dl = dst + static_cast<size_t>(y) * w;
        const uint32_t* pl = tv ? prevFrame + static_cast<size_t>(y) * w
                                : nullptr;
        renderLine(sl, dl, w, mode, pl, tv, phaseOffset);
    }
}

} // namespace pom2

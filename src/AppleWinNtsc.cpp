// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleWin-style NTSC simulation. See AppleWinNtsc.h for the algorithm
// summary and credits — this file holds the implementation. All filter
// coefficients are standard 2nd-order IIR values from any DSP textbook;
// no AppleWin source code is copied.

#include "AppleWinNtsc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace pom2 {

namespace {

constexpr int kPhases = 4;       // 4× subcarrier alignment
constexpr int kHistBits = 12;    // 12-bit sample history
constexpr int kHistSize = 1 << kHistBits;  // 4096
constexpr float kPi = 3.14159265358979323846f;

// Pre-computed LUT: chromaLut[phase][history12] -> RGBA8 packed
// (0xAABBGGRR — same convention as Apple2Display::frame).
uint32_t g_chromaLut[kPhases][kHistSize];
// Smaller "Idealized" palette: bypasses the IIR transient. Indexed by
// the 4-bit nibble that fills the current dot's 4-sample window.
uint32_t g_idealizedLut[kPhases][16];
std::atomic<bool> g_initDone{false};

// Standard NTSC YIQ → RGB (FCC §73.682), matching what NtscPostProcessor
// uses on the GPU side. Identical floats to ensure the two POM2 NTSC
// implementations sit in the same colour space.
inline void yiqToRgb(float y, float i, float q,
                     uint8_t& r, uint8_t& g, uint8_t& b)
{
    float fr = y + 0.956f * i + 0.621f * q;
    float fg = y - 0.272f * i - 0.647f * q;
    float fb = y - 1.106f * i + 1.703f * q;
    fr = std::clamp(fr, 0.0f, 1.0f);
    fg = std::clamp(fg, 0.0f, 1.0f);
    fb = std::clamp(fb, 0.0f, 1.0f);
    r = static_cast<uint8_t>(fr * 255.0f);
    g = static_cast<uint8_t>(fg * 255.0f);
    b = static_cast<uint8_t>(fb * 255.0f);
}

inline uint32_t packRGBA(uint8_t r, uint8_t g, uint8_t b)
{
    return 0xFF000000u
         | (static_cast<uint32_t>(b) << 16)
         | (static_cast<uint32_t>(g) << 8)
         |  static_cast<uint32_t>(r);
}

// Build chromaLut[phase][history12].
//
// For each (phase, history) pair we:
//   1. Decode the 12 bits back into a sample window (oldest → newest).
//   2. Pass the window through the signal-shaping IIR.
//   3. The OUTPUT of the filter at the "current" sample (rightmost) is
//      the composite voltage Y_t. We also pull I/Q at this sample by
//      multiplying the previous N samples with sin/cos at the local
//      subcarrier phase and averaging.
//   4. Convert YIQ → RGB and pack.
void buildChromaLut()
{
    // Window length used for I/Q averaging — same order of magnitude
    // as the existing OpenEmulator shader (8 taps each side). 12 bits
    // of history naturally caps us at 12 samples here.
    constexpr int N = 12;

    // Two filters extract Y and chroma from the same 12-sample window
    // with different bandwidths — close to what AppleWin's separate
    // luma/chroma IIRs do, but expressed as gaussian-weighted moving
    // averages because that's what fits cleanly into a static LUT.
    //
    //   - Luma:   narrow gaussian (sigmaY = 1.5) — sharp, high contrast.
    //   - Chroma: wide gaussian (sigmaC = 3.0) and DC removal so a
    //             constant input produces zero I/Q. Wider window =
    //             cleaner subcarrier extraction.
    //
    // The "current" sample is k = N-1 (offset 0); the chroma weights
    // sit symmetrically around it by treating k = N-1 as the centre
    // of a virtual 2N-tap kernel — we only have 12 past samples so we
    // mirror the past taps into the future, a standard trick when the
    // forward window is unavailable in real-time decoding.
    // Bit-ordering convention (must match renderLine):
    //   hist bit 0 = NEWEST sample (= the dot just shifted in)
    //   hist bit N-1 = OLDEST sample.
    // Centred-window post-processing means the "current" output pixel
    // sits at offset N/2 from the newest sample — i.e. the chroma
    // phase reference is the centre, not the trailing edge.
    constexpr int kCentre = N / 2;   // 6
    for (int phase = 0; phase < kPhases; ++phase) {
        for (int hist = 0; hist < kHistSize; ++hist) {
            float raw[N];
            for (int k = 0; k < N; ++k) {
                raw[k] = (hist >> k) & 1u ? 1.0f : 0.0f;
            }

            // Luma — narrow gaussian centred on kCentre.
            const float sigmaY = 1.5f;
            float ySum = 0.0f, wYsum = 0.0f;
            for (int k = 0; k < N; ++k) {
                const float dx = static_cast<float>(k - kCentre);
                const float w  = std::exp(-0.5f * dx * dx / (sigmaY * sigmaY));
                ySum  += raw[k] * w;
                wYsum += w;
            }
            const float Y = ySum / wYsum;

            // Chroma — wider window, DC removed, subcarrier angle
            // referenced to the centre sample's absolute phase.
            const float sigmaC = 3.0f;
            float iSum = 0.0f, qSum = 0.0f, wCsum = 0.0f;
            for (int k = 0; k < N; ++k) {
                const int offset = k - kCentre;  // -kCentre .. +(N-1-kCentre)
                const float dx = static_cast<float>(offset);
                const float w  = std::exp(-0.5f * dx * dx / (sigmaC * sigmaC));
                const float ang = 0.5f * kPi
                                * static_cast<float>(phase + offset);
                const float ac = raw[k] - Y;
                iSum  += ac * std::sin(ang) * w;
                qSum  += ac * std::cos(ang) * w;
                wCsum += w;
            }
            // Chroma saturation: amplify the AC content to compensate
            // for the broad filter's natural attenuation. Tuned by eye
            // against known artifact-colour patterns ($55 → purple,
            // $2A → green) — matches the visual signature of AppleWin's
            // "Color Monitor (NTSC)" output.
            constexpr float kChromaSat = 10.0f;
            const float I = (iSum / wCsum) * kChromaSat;
            const float Q = (qSum / wCsum) * kChromaSat;

            // Mild contrast boost — keeps blacks black and whites near 1.
            const float Yc = std::clamp((Y - 0.5f) * 1.15f + 0.5f,
                                        0.0f, 1.0f);

            uint8_t r, g, b;
            yiqToRgb(Yc, I, Q, r, g, b);
            g_chromaLut[phase][hist] = packRGBA(r, g, b);
        }
    }
}

// Build idealizedLut[phase][nibble]. The Idealized path skips the IIR
// transient and feeds a clean 4-bit pattern through the same YIQ
// demodulation, producing saturated artifact colours without the
// roll-off / ringing of the Monitor sub-mode.
void buildIdealizedLut()
{
    // Same bit-ordering convention as chromaLut: nib bit 0 = NEWEST
    // sample, bit 3 = oldest. We DC-remove the 4-sample mean before
    // demod so a constant pattern (0x0 or 0xF) produces zero chroma.
    for (int phase = 0; phase < kPhases; ++phase) {
        for (int nib = 0; nib < 16; ++nib) {
            float s[4];
            float Y = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s[k] = (nib >> k) & 1u ? 1.0f : 0.0f;
                Y   += s[k];
            }
            Y *= 0.25f;
            float iSum = 0.0f, qSum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                // Centre the 4-sample window around the output pixel:
                // bit 0 = newest (centre+2), bit 3 = oldest (centre-1).
                const int offset = k - 2;
                const float ang = 0.5f * kPi
                                * static_cast<float>(phase + offset);
                const float ac  = s[k] - Y;
                iSum += ac * std::sin(ang);
                qSum += ac * std::cos(ang);
            }
            // Idealized's whole point is saturated, punchy artifact
            // colours that pop on a modern flat panel — boost chroma
            // hard, no low-pass attenuation to fight.
            constexpr float kIdealSat = 8.0f;
            const float I = iSum * 0.25f * kIdealSat;
            const float Q = qSum * 0.25f * kIdealSat;
            uint8_t r, g, b;
            yiqToRgb(Y, I, Q, r, g, b);
            g_idealizedLut[phase][nib] = packRGBA(r, g, b);
        }
    }
}

} // namespace

void AppleWinNtsc::ensureInitialized()
{
    // std::call_once makes the header's "safe to call from any thread"
    // guarantee actually hold: the old check-then-act let two concurrent
    // first-callers both run the build* writers and race on the shared LUTs.
    static std::once_flag initFlag;
    std::call_once(initFlag, [] {
        buildChromaLut();
        buildIdealizedLut();
        g_initDone.store(true, std::memory_order_release);
    });
}

void AppleWinNtsc::renderLine(const uint8_t* src,
                              uint32_t*      dst,
                              int            w,
                              SubMode        mode,
                              const uint32_t* prevLine,
                              bool           prevValid)
{
    ensureInitialized();

    // 12-bit shift register over the signal stream. The chroma kernel
    // is centred on the output pixel — when we've shifted 12 fresh
    // samples in, the OUTPUT pixel for column (x - 6) sits in the
    // middle of the window. Half-window delay = kCenterDelay; we mirror
    // the left edge to avoid black-ramp transients, and run an extra
    // kCenterDelay samples past the right edge by sustaining the last
    // signal value, so the rightmost pixels still see a populated
    // window.
    constexpr int kCenterDelay = 6;
    uint32_t hist = src[0] ? 0x0FFFu : 0u;

    if (mode == SubMode::Idealized) {
        for (int x = 0; x < w; ++x) {
            hist = (hist << 1) & 0x0FFFu;
            if (src[x]) hist |= 1u;
            const unsigned nib = (hist >> 0) & 0x0Fu;
            dst[x] = g_idealizedLut[x & 3][nib];
        }
        return;
    }

    // Prime the window with the first kCenterDelay samples so the very
    // first output pixel already has populated future context.
    for (int k = 0; k < kCenterDelay && k < w; ++k) {
        hist = (hist << 1) & 0x0FFFu;
        if (src[k]) hist |= 1u;
    }
    for (int x = 0; x < w; ++x) {
        const int readAhead = x + kCenterDelay;
        const uint8_t s = (readAhead < w) ? src[readAhead]
                                          : src[w - 1];
        hist = (hist << 1) & 0x0FFFu;
        if (s) hist |= 1u;
        dst[x] = g_chromaLut[x & 3][hist];
    }

    // Tv sub-mode: 50% blend with the previous frame's same scanline.
    // The dst we just wrote is the "current monitor frame"; before it
    // ships out, fold half of the previous one in. Done in-place.
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
                               const uint32_t* prevFrame)
{
    const bool tv = (mode == SubMode::Tv) && (prevFrame != nullptr);
    for (int y = 0; y < h; ++y) {
        const uint8_t* sl = src + static_cast<size_t>(y) * w;
        uint32_t*      dl = dst + static_cast<size_t>(y) * w;
        const uint32_t* pl = tv ? prevFrame + static_cast<size_t>(y) * w
                                : nullptr;
        renderLine(sl, dl, w, mode, pl, tv);
    }
}

} // namespace pom2

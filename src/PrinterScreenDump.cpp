// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrinterScreenDump implementation. See the header for why this synthesises a
// wire format rather than painting the page.

#include "PrinterScreenDump.h"

#include <algorithm>

namespace pom2 {

namespace {

constexpr uint8_t kEsc = 0x1B;

/// Rows of dots the head lays down in one pass. Nine wires exist, but the
/// bit-image protocol addresses eight — the ninth is the descender/underline
/// wire and takes no part in graphics.
constexpr int kBandRows = 8;

/// Rec. 601 luma, the same weighting POM2's mono display modes use, so a
/// screen that looks light on screen prints light.
inline int luminance(uint32_t argb)
{
    const int r = static_cast<int>((argb >> 16) & 0xFF);
    const int g = static_cast<int>((argb >> 8) & 0xFF);
    const int b = static_cast<int>(argb & 0xFF);
    return (r * 77 + g * 151 + b * 28) >> 8;
}

/// Four ASCII digits, which is how the C. Itoh family spells a count.
void pushCount4(std::vector<uint8_t>& out, int n)
{
    n = std::max(0, std::min(9999, n));
    out.push_back(static_cast<uint8_t>('0' + (n / 1000) % 10));
    out.push_back(static_cast<uint8_t>('0' + (n / 100) % 10));
    out.push_back(static_cast<uint8_t>('0' + (n / 10) % 10));
    out.push_back(static_cast<uint8_t>('0' + n % 10));
}

/// Two ASCII digits — `ESC T nn`, line spacing in nn/144 in.
void pushCount2(std::vector<uint8_t>& out, int n)
{
    n = std::max(0, std::min(99, n));
    out.push_back(static_cast<uint8_t>('0' + (n / 10) % 10));
    out.push_back(static_cast<uint8_t>('0' + n % 10));
}

bool litFraction(const uint32_t* pixels, int w, int h, int stride,
                 int threshold, double& fracOut)
{
    if (!pixels || w <= 0 || h <= 0) return false;
    uint64_t lit = 0;
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = pixels + static_cast<size_t>(y) * stride;
        for (int x = 0; x < w; ++x)
            if (luminance(row[x]) >= threshold) ++lit;
    }
    fracOut = static_cast<double>(lit) /
              (static_cast<double>(w) * static_cast<double>(h));
    return true;
}

} // namespace

bool screenDumpWouldInvert(const uint32_t* pixels, int w, int h, int stride,
                           const ScreenDumpOptions& opt)
{
    if (!opt.autoInvert) return opt.invert;
    double frac = 0.0;
    if (!litFraction(pixels, w, h, stride, opt.threshold, frac)) return false;
    // A mostly-lit screen is a text screen (or an inverse-video one): print
    // the DARK pixels, or the page comes out almost solid black and the
    // ribbon pays for it.
    return frac > 0.5;
}

void buildScreenDumpImageWriter(const uint32_t* pixels, int w, int h,
                                int stride, const ScreenDumpOptions& opt,
                                std::vector<uint8_t>& out)
{
    if (!pixels || w <= 0 || h <= 0) return;
    if (stride <= 0) stride = w;

    const bool invert = screenDumpWouldInvert(pixels, w, h, stride, opt);

    // 72 dpi bit image: `ESC n` sets 9 cpi, whose graphics density is 72 dpi
    // horizontally, so one screen pixel becomes one dot and the aspect ratio
    // survives (the band feed below is the vertical half of that).
    out.push_back(kEsc);
    out.push_back('n');

    // 16/144 in = 8 dots at 72 dpi — exactly one band, so consecutive bands
    // abut with no white seam and no overlap.
    const uint8_t feed[2] = { kEsc, 'T' };

    for (int bandTop = 0; bandTop < h; bandTop += kBandRows) {
        // One column byte per horizontal pixel.
        out.push_back(kEsc);
        out.push_back('G');
        pushCount4(out, w);

        for (int x = 0; x < w; ++x) {
            uint8_t col = 0;
            for (int bit = 0; bit < kBandRows; ++bit) {
                const int y = bandTop + bit;
                if (y >= h) break;              // last band is short
                const uint32_t px = pixels[static_cast<size_t>(y) * stride + x];
                bool on = luminance(px) >= opt.threshold;
                if (invert) on = !on;
                // Bit 0 is the TOP dot of the band on this family — the
                // opposite of Epson's ESC *, which is why the two protocols
                // cannot share a packer without a flag.
                if (on) col |= static_cast<uint8_t>(1u << bit);
            }
            out.push_back(col);
        }

        out.push_back(feed[0]);
        out.push_back(feed[1]);
        pushCount2(out, 16);
        out.push_back('\r');
        out.push_back('\n');
    }

    if (opt.formFeed) out.push_back(0x0C);
}

void buildScreenDumpEpson(const uint32_t* pixels, int w, int h, int stride,
                          const ScreenDumpOptions& opt,
                          std::vector<uint8_t>& out)
{
    if (!pixels || w <= 0 || h <= 0) return;
    if (stride <= 0) stride = w;

    const bool invert = screenDumpWouldInvert(pixels, w, h, stride, opt);

    // ESC * mode 5 is 72 dpi — one screen pixel per dot, so the aspect ratio
    // survives, the same reasoning as `ESC n` on the C. Itoh side.
    constexpr uint8_t kMode72Dpi = 5;

    for (int bandTop = 0; bandTop < h; bandTop += kBandRows) {
        out.push_back(kEsc);
        out.push_back('*');
        out.push_back(kMode72Dpi);
        // TWO BINARY BYTES, low first — not four ASCII digits.
        out.push_back(static_cast<uint8_t>(w & 0xFF));
        out.push_back(static_cast<uint8_t>((w >> 8) & 0xFF));

        for (int x = 0; x < w; ++x) {
            uint8_t col = 0;
            for (int bit = 0; bit < kBandRows; ++bit) {
                const int y = bandTop + bit;
                if (y >= h) break;
                const uint32_t px = pixels[static_cast<size_t>(y) * stride + x];
                bool on = luminance(px) >= opt.threshold;
                if (invert) on = !on;
                // Bit 7 is the TOP dot here — the opposite of ESC G.
                if (on) col |= static_cast<uint8_t>(0x80u >> bit);
            }
            out.push_back(col);
        }

        // 24/216 in = 8 dots at 72 dpi, so bands abut. ESC 3 takes ONE raw
        // byte, where the C. Itoh ESC T takes two ASCII digits.
        out.push_back(kEsc);
        out.push_back('3');
        out.push_back(24);
        out.push_back('\r');
        out.push_back('\n');
    }

    if (opt.formFeed) out.push_back(0x0C);
}

} // namespace pom2

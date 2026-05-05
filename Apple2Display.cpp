// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Apple2Display.h"
#include "Memory.h"

#include <array>
#include <cstring>

Apple2Display::Apple2Display()
    : frame(kWidth * kHeight, 0xFF000000)
{
}

uint16_t Apple2Display::textRowAddress(int y, bool page2)
{
    // Apple II text/lo-res row interleave: addr = base + 0x80*(y%8) +
    // 0x28*(y/8). The 8 KB DRAM dance the Apple I never had — Woz reused
    // the row counter to refresh dynamic RAM.
    const uint16_t base = page2 ? 0x0800 : 0x0400;
    return static_cast<uint16_t>(base + 0x80 * (y & 7) + 0x28 * (y >> 3));
}

uint16_t Apple2Display::hgrRowAddress(int y, bool page2)
{
    // HGR formula: addr = base + 0x400*(y%8) + 0x80*((y/8)%8) + 0x28*(y/64).
    // Same trick as text mode but the inner counter goes through 8 banks
    // of 8 sub-pages of 40 bytes (3 sub-pages × 64 lines = 192).
    const uint16_t base = page2 ? 0x4000 : 0x2000;
    return static_cast<uint16_t>(base
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

void Apple2Display::render(Memory& mem)
{
    ++frameCounter;     // drives the FLASH-attribute animation in renderText
    const auto state = mem.getDisplayState();
    if (state.textMode) {
        renderText(mem, 0, 24);
    } else if (state.hiRes) {
        renderHiRes(mem, 0, 192);
        if (state.mixedMode) renderText(mem, 20, 24);  // last 4 rows = text
    } else {
        renderLoRes(mem, 0, 48);
        if (state.mixedMode) renderText(mem, 20, 24);
    }
}

// ─── Text mode ────────────────────────────────────────────────────────────

// Built-in 5×7 monospaced ASCII font, packed as 8 bytes per glyph (top→bottom).
// Bits 0-4 = pixel pattern; bit 0 is the leftmost dot. Only the printable
// range ($20-$7F) is fleshed out — control codes render as a checker pattern.
// Characters not commonly used by Apple II text output (lowercase) inherit
// from their uppercase counterparts; original Apple II only had uppercase
// anyway so this matches the visual.
static const uint8_t kAscii5x7[96 * 8] = {
    // 0x20 ' '
    0,0,0,0,0,0,0,0,
    // 0x21 '!'
    0x04,0x04,0x04,0x04,0x04,0x00,0x04,0x00,
    // 0x22 '"'
    0x0A,0x0A,0x0A,0,0,0,0,0,
    // 0x23 '#'
    0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A,0,
    // 0x24 '$'
    0x04,0x0F,0x14,0x0E,0x05,0x1E,0x04,0,
    // 0x25 '%'
    0x19,0x19,0x02,0x04,0x08,0x13,0x13,0,
    // 0x26 '&'
    0x08,0x14,0x14,0x08,0x15,0x12,0x0D,0,
    // 0x27 '\''
    0x04,0x04,0x08,0,0,0,0,0,
    // 0x28 '('
    0x02,0x04,0x08,0x08,0x08,0x04,0x02,0,
    // 0x29 ')'
    0x08,0x04,0x02,0x02,0x02,0x04,0x08,0,
    // 0x2A '*'
    0x00,0x04,0x15,0x0E,0x15,0x04,0x00,0,
    // 0x2B '+'
    0x00,0x04,0x04,0x1F,0x04,0x04,0x00,0,
    // 0x2C ','
    0,0,0,0,0,0x04,0x04,0x08,
    // 0x2D '-'
    0x00,0x00,0x00,0x1F,0x00,0x00,0x00,0,
    // 0x2E '.'
    0,0,0,0,0,0x0C,0x0C,0,
    // 0x2F '/'
    0x01,0x01,0x02,0x04,0x08,0x10,0x10,0,
    // 0x30 '0'
    0x0E,0x11,0x13,0x15,0x19,0x11,0x0E,0,
    // 0x31 '1'
    0x04,0x0C,0x04,0x04,0x04,0x04,0x0E,0,
    // 0x32 '2'
    0x0E,0x11,0x01,0x02,0x04,0x08,0x1F,0,
    // 0x33 '3'
    0x0E,0x11,0x01,0x06,0x01,0x11,0x0E,0,
    // 0x34 '4'
    0x02,0x06,0x0A,0x12,0x1F,0x02,0x02,0,
    // 0x35 '5'
    0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E,0,
    // 0x36 '6'
    0x06,0x08,0x10,0x1E,0x11,0x11,0x0E,0,
    // 0x37 '7'
    0x1F,0x01,0x02,0x04,0x08,0x08,0x08,0,
    // 0x38 '8'
    0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E,0,
    // 0x39 '9'
    0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C,0,
    // 0x3A ':'
    0,0,0x0C,0x0C,0,0x0C,0x0C,0,
    // 0x3B ';'
    0,0,0x0C,0x0C,0,0x0C,0x04,0x08,
    // 0x3C '<'
    0x02,0x04,0x08,0x10,0x08,0x04,0x02,0,
    // 0x3D '='
    0,0,0x1F,0,0x1F,0,0,0,
    // 0x3E '>'
    0x08,0x04,0x02,0x01,0x02,0x04,0x08,0,
    // 0x3F '?'
    0x0E,0x11,0x01,0x02,0x04,0x00,0x04,0,
    // 0x40 '@'
    0x0E,0x11,0x01,0x0D,0x15,0x15,0x0E,0,
    // 0x41 'A'
    0x0E,0x11,0x11,0x11,0x1F,0x11,0x11,0,
    // 0x42 'B'
    0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E,0,
    // 0x43 'C'
    0x0E,0x11,0x10,0x10,0x10,0x11,0x0E,0,
    // 0x44 'D'
    0x1C,0x12,0x11,0x11,0x11,0x12,0x1C,0,
    // 0x45 'E'
    0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F,0,
    // 0x46 'F'
    0x1F,0x10,0x10,0x1E,0x10,0x10,0x10,0,
    // 0x47 'G'
    0x0E,0x11,0x10,0x17,0x11,0x11,0x0F,0,
    // 0x48 'H'
    0x11,0x11,0x11,0x1F,0x11,0x11,0x11,0,
    // 0x49 'I'
    0x0E,0x04,0x04,0x04,0x04,0x04,0x0E,0,
    // 0x4A 'J'
    0x07,0x02,0x02,0x02,0x02,0x12,0x0C,0,
    // 0x4B 'K'
    0x11,0x12,0x14,0x18,0x14,0x12,0x11,0,
    // 0x4C 'L'
    0x10,0x10,0x10,0x10,0x10,0x10,0x1F,0,
    // 0x4D 'M'
    0x11,0x1B,0x15,0x15,0x11,0x11,0x11,0,
    // 0x4E 'N'
    0x11,0x11,0x19,0x15,0x13,0x11,0x11,0,
    // 0x4F 'O'
    0x0E,0x11,0x11,0x11,0x11,0x11,0x0E,0,
    // 0x50 'P'
    0x1E,0x11,0x11,0x1E,0x10,0x10,0x10,0,
    // 0x51 'Q'
    0x0E,0x11,0x11,0x11,0x15,0x12,0x0D,0,
    // 0x52 'R'
    0x1E,0x11,0x11,0x1E,0x14,0x12,0x11,0,
    // 0x53 'S'
    0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E,0,
    // 0x54 'T'
    0x1F,0x04,0x04,0x04,0x04,0x04,0x04,0,
    // 0x55 'U'
    0x11,0x11,0x11,0x11,0x11,0x11,0x0E,0,
    // 0x56 'V'
    0x11,0x11,0x11,0x11,0x11,0x0A,0x04,0,
    // 0x57 'W'
    0x11,0x11,0x11,0x15,0x15,0x15,0x0A,0,
    // 0x58 'X'
    0x11,0x11,0x0A,0x04,0x0A,0x11,0x11,0,
    // 0x59 'Y'
    0x11,0x11,0x11,0x0A,0x04,0x04,0x04,0,
    // 0x5A 'Z'
    0x1F,0x01,0x02,0x04,0x08,0x10,0x1F,0,
    // 0x5B '['
    0x0E,0x08,0x08,0x08,0x08,0x08,0x0E,0,
    // 0x5C '\\'
    0x10,0x10,0x08,0x04,0x02,0x01,0x01,0,
    // 0x5D ']'
    0x0E,0x02,0x02,0x02,0x02,0x02,0x0E,0,
    // 0x5E '^'
    0x04,0x0A,0x11,0,0,0,0,0,
    // 0x5F '_'
    0,0,0,0,0,0,0x1F,0,
    // 0x60 '`'
    0x08,0x04,0x02,0,0,0,0,0,
    // The lowercase range $61-$7A isn't backed by per-glyph patterns yet;
    // resolveGlyph() falls back to the uppercase glyph when a //e program
    // writes a lowercase byte. Drop a real //e character ROM into
    // roms/apple2_char.rom for accurate lowercase rendering.
};

// Map a screen byte to a glyph row pattern + video attributes.
//   $00-$3F  inverse   ─ low 6 bits = char index (always inverse)
//   $40-$7F  flashing  ─ low 6 bits = char index (alternates inverse/normal
//                        at ~1 Hz — drives the Monitor cursor blink and
//                        any inverse-blinking spaces left behind by
//                        Applesoft when it moves to a new line)
//   $80-$FF  normal    ─ low 7 bits = ASCII (//e exposes lowercase here)
static void resolveGlyph(uint8_t screenByte, uint8_t out[8],
                         bool& invert, bool& flash)
{
    uint8_t ascii;
    flash = false;
    if (screenByte & 0x80) {
        invert = false;
        ascii  = screenByte & 0x7F;
    } else {
        invert = true;
        flash  = (screenByte & 0x40) != 0;   // bit 6 set → FLASH attribute
        const uint8_t idx6 = screenByte & 0x3F;
        ascii = (idx6 < 0x20) ? static_cast<uint8_t>(0x40 + idx6) : idx6;
    }

    // Lowercase fallback to uppercase while no character ROM is loaded.
    if (ascii >= 0x61 && ascii <= 0x7A) ascii = static_cast<uint8_t>(ascii - 0x20);

    if (ascii >= 0x20 && ascii <= 0x60) {
        std::memcpy(out, &kAscii5x7[(ascii - 0x20) * 8], 8);
    } else {
        const uint8_t box[8] = { 0x1F, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1F, 0 };
        std::memcpy(out, box, 8);
    }
}

void Apple2Display::renderText(Memory& mem, int firstRow, int lastRow)
{
    const auto state = mem.getDisplayState();
    const uint8_t* ram = mem.data();

    // Flash phase: 0 = invert as-stored, 1 = flip back to normal. Toggles
    // every kFlashHalfPeriodFrames frames (~0.5 s at 60 Hz → 1 Hz cycle,
    // matches Apple II silicon).
    const bool flashPhase = (frameCounter / kFlashHalfPeriodFrames) & 1u;

    for (int row = firstRow; row < lastRow; ++row) {
        const uint16_t rowAddr = textRowAddress(row, state.page2);
        for (int col = 0; col < 40; ++col) {
            uint8_t glyph[8];
            bool invert = false;
            bool flash  = false;
            resolveGlyph(ram[rowAddr + col], glyph, invert, flash);
            if (flash && flashPhase) invert = !invert;

            // Each cell is 7 wide × 8 tall. Bits 0-4 of the glyph are the 5
            // active pixels with one column of leading and one of trailing
            // padding, matching the Apple II character cell width. In
            // inverse mode the whole cell flips, padding columns included,
            // so a flashing space ($60 — the Monitor's cursor) renders as
            // a solid block.
            const int cellX = col * 7;
            const int cellY = row * 8;
            for (int gy = 0; gy < 8; ++gy) {
                const uint8_t row8 = glyph[gy];
                for (int gx = 0; gx < 7; ++gx) {
                    bool lit = (gx >= 1 && gx <= 5)
                            && ((row8 >> (5 - gx)) & 1);
                    if (invert) lit = !lit;
                    const int px = cellX + gx;
                    const int py = cellY + gy;
                    frame[py * kWidth + px] = lit ? 0xFFFFFFFFu : 0xFF000000u;
                }
            }
        }
    }
}

// ─── Lo-res mode ──────────────────────────────────────────────────────────

const uint32_t Apple2Display::kLoResPalette[16] = {
    0xFF000000, // 0 black
    0xFF722640, // 1 magenta
    0xFF40337F, // 2 dark blue
    0xFFE434FE, // 3 purple
    0xFF0E5940, // 4 dark green
    0xFF808080, // 5 grey 1
    0xFF1B9AFE, // 6 medium blue
    0xFFBFB3FF, // 7 light blue
    0xFF404C00, // 8 brown
    0xFFE46501, // 9 orange
    0xFF808080, // 10 grey 2
    0xFFF1A6BF, // 11 pink
    0xFF1BCB01, // 12 green
    0xFFBFCC80, // 13 yellow
    0xFF40D9BF, // 14 aqua
    0xFFFFFFFF, // 15 white
};

void Apple2Display::renderLoRes(Memory& mem, int firstRow, int lastRow)
{
    // Lo-res draws 40 columns × 48 rows of 7×4 colour blocks. Each text
    // byte stores TWO blocks: low nibble is the upper block, high nibble
    // the lower one.
    const auto state = mem.getDisplayState();
    const uint8_t* ram = mem.data();

    // Each lo-res row corresponds to half a text row (4 scanlines).
    for (int blockRow = firstRow; blockRow < lastRow; ++blockRow) {
        const int textRow = blockRow / 2;
        const bool upperHalf = (blockRow % 2 == 0);
        const uint16_t rowAddr = textRowAddress(textRow, state.page2);
        for (int col = 0; col < 40; ++col) {
            const uint8_t b = ram[rowAddr + col];
            const uint8_t nibble = upperHalf ? (b & 0x0F) : (b >> 4);
            const uint32_t rgb = kLoResPalette[nibble];
            const int x0 = col * 7;
            const int y0 = blockRow * 4;
            for (int dy = 0; dy < 4; ++dy)
                for (int dx = 0; dx < 7; ++dx)
                    frame[(y0 + dy) * kWidth + (x0 + dx)] = rgb;
        }
    }
}

// ─── Hi-res mode ──────────────────────────────────────────────────────────
//
// Apple II HGR NTSC artifact colour, ported from Uncle Bernie's GEN2 card
// in POM1. Three passes per scanline:
//
//   1. LUT lookup — each (parity, byte) maps to 7 RGBA pixels. The table
//      assumes the byte is "isolated" — neighbours treated as off — so
//      bit 6 of byte N and bit 0 of byte N+1 may need a fix-up below.
//   2. Inter-byte seam fix-up — when bit 6 of the current byte AND bit 0
//      of the next byte are both lit, both seam pixels paint white. This
//      is the only neighbour-dependent case the LUT can't cover.
//   3. Optional horizontal glow — black pixels next to lit neighbours
//      pick up a soft halo. Loose stand-in for NTSC chroma bandwidth
//      smear; toggleable via setHiResGlow().
//
// The byte/bit/pixel convention follows Apple II silicon: the LSB of
// each HGR byte (bit 0) is the LEFTMOST displayed pixel; bit 6 is the
// rightmost. Bit 7 is the per-byte palette flag (0 = violet/green pair,
// 1 = blue/orange pair). "Even pixel parity" relative to the screen's
// absolute X selects which colour of the pair shows for a single
// isolated lit pixel; lit pixels with a lit neighbour merge to white.

namespace {

constexpr uint32_t pack(uint8_t r, uint8_t g, uint8_t b)
{
    // Memory order R, G, B, A → little-endian uint32: (A<<24)|(B<<16)|(G<<8)|R.
    // GL_RGBA + GL_UNSIGNED_BYTE consumes the bytes in that order.
    return (uint32_t(0xFF) << 24)
         | (uint32_t(b)    << 16)
         | (uint32_t(g)    << 8)
         |  uint32_t(r);
}

constexpr uint32_t kHiResBlack  = 0xFF000000u;
constexpr uint32_t kHiResWhite  = 0xFFFFFFFFu;
constexpr uint32_t kHiResViolet = pack(148,  33, 246);  // group 1, even screenX
constexpr uint32_t kHiResGreen  = pack( 20, 245,  60);  // group 1, odd screenX
constexpr uint32_t kHiResBlue   = pack( 20, 207, 253);  // group 2, even screenX
constexpr uint32_t kHiResOrange = pack(255, 106,  60);  // group 2, odd screenX

using HgrPixelRow   = std::array<uint32_t, 7>;
using HgrPixelTable = std::array<HgrPixelRow, 512>;

constexpr uint32_t computeIsolatedPixel(int byte, int bit, int colParity)
{
    const bool on = (byte & (1 << bit)) != 0;
    if (!on) return kHiResBlack;
    const bool prevOn = (bit > 0) && ((byte & (1 << (bit - 1))) != 0);
    const bool nextOn = (bit < 6) && ((byte & (1 << (bit + 1))) != 0);
    if (prevOn || nextOn) return kHiResWhite;
    const bool group2 = (byte & 0x80) != 0;
    const bool even   = ((colParity + bit) & 1) == 0;
    if (!group2) return even ? kHiResViolet : kHiResGreen;
    return even ? kHiResBlue : kHiResOrange;
}

const HgrPixelTable& hgrPixelTable()
{
    // 14 KB lazy-static table — built once per process. Index =
    // (colParity << 8) | byte. colParity = parity of the absolute
    // screenX where the byte starts. Since each byte is 7 pixels wide
    // and 7 is odd, colParity == col & 1 (odd column → odd start).
    static const HgrPixelTable table = []{
        HgrPixelTable t{};
        for (int parity = 0; parity < 2; ++parity)
            for (int byte = 0; byte < 256; ++byte)
                for (int bit = 0; bit < 7; ++bit)
                    t[(parity << 8) | byte][bit] =
                        computeIsolatedPixel(byte, bit, parity);
        return t;
    }();
    return table;
}

} // namespace

void Apple2Display::renderHiRes(Memory& mem, int firstScanline, int lastScanline)
{
    const auto state = mem.getDisplayState();
    const uint8_t* ram = mem.data();
    const auto& table = hgrPixelTable();

    // Per-scanline scratch — LUT output before glow. 280 × 4 B = 1.1 KB
    // on the stack, well below the default 8 MB thread stack.
    std::array<uint32_t, kWidth> raw;

    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, state.page2);

        // Pass 1: LUT — 40 bytes × 7 pixels.
        for (int col = 0; col < 40; ++col) {
            const uint8_t b = ram[rowAddr + col];
            const int parity = col & 1;
            std::memcpy(raw.data() + col * 7,
                        table[(parity << 8) | b].data(),
                        sizeof(HgrPixelRow));
        }

        // Pass 2: 39 inter-byte seams. The LUT was built assuming no
        // external neighbour, so the only case it gets wrong is when bit
        // 6 of the current byte AND bit 0 of the next byte are both lit
        // — both seam pixels then paint white (matches HGR silicon).
        for (int col = 0; col < 39; ++col) {
            const uint8_t cur = ram[rowAddr + col];
            const uint8_t nxt = ram[rowAddr + col + 1];
            if ((cur & 0x40) && (nxt & 0x01)) {
                raw[col * 7 + 6] = kHiResWhite;
                raw[col * 7 + 7] = kHiResWhite;
            }
        }

        // Pass 3: glow into the final framebuffer — or just memcpy if
        // the user disabled the halo.
        uint32_t* outRow = frame.data() + static_cast<size_t>(y) * kWidth;
        if (hgrGlowEnabled) {
            applyHgrGlow(raw.data(), outRow);
        } else {
            std::memcpy(outRow, raw.data(), sizeof(raw));
        }
    }
}

void Apple2Display::applyHgrGlow(const uint32_t* src, uint32_t* dst)
{
    // Horizontal-only additive glow. Each lit lateral neighbour
    // contributes 9/20 of its colour into the current black pixel, summed
    // and clamped per channel. A black pixel sandwiched between two
    // identical lit pixels reaches 90 % of the source colour — bright
    // enough to read as a CRT halo without bleeding into adjacent rows.
    constexpr int kGlowNum = 9;
    constexpr int kGlowDen = 20;

    for (int x = 0; x < kWidth; ++x) {
        const uint32_t c = src[x];
        // Lit pixel? Pass through unchanged. Bit-test on the colour bytes
        // (RGB only — alpha bits stay 0xFF for both lit and black).
        if ((c & 0x00FFFFFFu) != 0) { dst[x] = c; continue; }

        const uint32_t L = (x > 0)          ? src[x - 1] : 0u;
        const uint32_t R = (x + 1 < kWidth) ? src[x + 1] : 0u;

        const int sr = int(L         & 0xFFu) + int(R         & 0xFFu);
        const int sg = int((L >> 8)  & 0xFFu) + int((R >> 8)  & 0xFFu);
        const int sb = int((L >> 16) & 0xFFu) + int((R >> 16) & 0xFFu);

        int r = (sr * kGlowNum) / kGlowDen;
        int g = (sg * kGlowNum) / kGlowDen;
        int b = (sb * kGlowNum) / kGlowDen;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;
        dst[x] = (uint32_t(0xFF) << 24)
               | (uint32_t(b)    << 16)
               | (uint32_t(g)    << 8)
               |  uint32_t(r);
    }
}

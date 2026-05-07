// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <algorithm>
#include <array>
#include <cstring>

Apple2Display::Apple2Display()
    : frame(kWidth * kHeight, 0xFF000000)
    , frame80(kWidth80 * kHeight, 0xFF000000)
    , persistenceL(kWidth * kHeight, 0)
{
}

void Apple2Display::setHiResMode(HiResMode m)
{
    if (m == hiResMode) return;
    hiResMode = m;
    // Clear the phosphor history so a residual amber afterglow doesn't
    // tint a freshly-selected green or colour mode for a few frames.
    std::fill(persistenceL.begin(), persistenceL.end(), 0);
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

    // IIe + 80COL active. Sub-cases:
    //   (a) Full-screen 80-col text          → frame80 only.
    //   (b) DHGR full-screen (HIRES + DHGR)  → frame80 only, no text.
    //   (c) DHGR + MIXED                     → DHGR top 160 + 80-col text rows 20..23.
    //   (d) Mixed HGR + 80-col text          → HGR top into frame, upscale 2× into
    //                                           frame80, then 80-col text rows 20..23.
    //   (e) Mixed lo-res + 80-col text       → same recipe with renderLoRes.
    // Anything else (HGR full-screen without DHGR, lo-res full-screen) keeps
    // the legacy 280-wide path even when 80COL is enabled — those modes
    // don't use the text page so the column count is irrelevant for the
    // framebuffer width.
    if (mem.isIIE() && state.eightyCol) {
        if (state.textMode) {
            renderText80(mem, 0, 24, state.altChar);
            useFrame80 = true;
            return;
        }
        if (state.hiRes && state.dhgr) {
            if (state.mixedMode) {
                renderDhgr(mem, 0, 160);
                renderText80(mem, 20, 24, state.altChar);
            } else {
                renderDhgr(mem, 0, 192);
            }
            useFrame80 = true;
            return;
        }
        if (state.mixedMode) {
            if (state.hiRes) {
                renderHiRes(mem, 0, 160);
                upscaleFrameToFrame80(0, 160);
            } else {
                renderLoRes(mem, 0, 40);   // 20 text rows × 2 lo-res rows
                upscaleFrameToFrame80(0, 160);
            }
            renderText80(mem, 20, 24, state.altChar);
            useFrame80 = true;
            return;
        }
    }

    useFrame80 = false;
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

// Palette verbatim from MAME `apple2video.cpp::apple2_palette[]` — the
// reference sRGB values calibrated against real Apple II hardware. Same
// 16 indices drive both lo-res blocks and the artefact LUT in
// renderHiRes() (the LUT looks up these very entries).
const uint32_t Apple2Display::kLoResPalette[16] = {
    0xFF000000, //  0 Black
    0xFF400BA7, //  1 Dark Red       rgb(0xa7, 0x0b, 0x40)
    0xFFF71C40, //  2 Dark Blue      rgb(0x40, 0x1c, 0xf7)
    0xFFFF28E6, //  3 Purple         rgb(0xe6, 0x28, 0xff)
    0xFF407400, //  4 Dark Green     rgb(0x00, 0x74, 0x40)
    0xFF808080, //  5 Dark Gray      rgb(0x80, 0x80, 0x80)
    0xFFFF9019, //  6 Medium Blue    rgb(0x19, 0x90, 0xff)
    0xFFFF9CBF, //  7 Light Blue     rgb(0xbf, 0x9c, 0xff)
    0xFF006340, //  8 Brown          rgb(0x40, 0x63, 0x00)
    0xFF006FE6, //  9 Orange         rgb(0xe6, 0x6f, 0x00)
    0xFF808080, // 10 Light Gray     rgb(0x80, 0x80, 0x80)  ← same as 5 in NTSC
    0xFFBF8BFF, // 11 Pink           rgb(0xff, 0x8b, 0xbf)
    0xFF00D719, // 12 Light Green    rgb(0x19, 0xd7, 0x00)
    0xFF08E3BF, // 13 Yellow         rgb(0xbf, 0xe3, 0x08)
    0xFFBFF458, // 14 Aquamarine     rgb(0x58, 0xf4, 0xbf)
    0xFFFFFFFF, // 15 White
};

// Le Chat Mauve / Video-7 lo-res palette. Lo-res is the place where the
// "two distinct grays" Chat Mauve trademark actually shows up on standard
// Apple II — because lo-res indexes its 16 colours directly from a 4-bit
// nibble in screen RAM, no chroma decoding involved. The digital RGB
// decoder turns the same nibble values into 16 visibly distinct colours,
// where NTSC composite collapses indices 5 and 10 onto the same grey
// because their phase signatures cancel through the chroma filter.
// Saturation is bumped vs. the NTSC //gs-corrected palette to match
// Péritel RGB drive levels.
const uint32_t Apple2Display::kChatMauveLoResPalette[16] = {
    0xFF000000, //  0 Black
    0xFF3300DD, //  1 Magenta / Dark Red
    0xFF990000, //  2 Dark Blue
    0xFFDD22DD, //  3 Violet
    0xFF228800, //  4 Dark Green
    0xFF555555, //  5 Dark Gray      ← distinct from index 10 (Chat Mauve trademark)
    0xFFFF4422, //  6 Medium Blue
    0xFFFFBB66, //  7 Light Blue
    0xFF005588, //  8 Brown
    0xFF0066FF, //  9 Orange
    0xFFAAAAAA, // 10 Light Gray     ← distinct from index 5 (Chat Mauve trademark)
    0xFFCC99FF, // 11 Pink
    0xFF22DD11, // 12 Light Green
    0xFF22FFFF, // 13 Yellow
    0xFFAAFF66, // 14 Aquamarine
    0xFFFFFFFF, // 15 White
};

void Apple2Display::renderLoRes(Memory& mem, int firstRow, int lastRow)
{
    // Lo-res draws 40 columns × 48 rows of 7×4 colour blocks. Each text
    // byte stores TWO blocks: low nibble is the upper block, high nibble
    // the lower one.
    const auto state = mem.getDisplayState();
    const uint8_t* ram = mem.data();

    // Palette selection. ChatMauveRGB swaps in the 16-colour Péritel
    // table — same indices, but indices 5 and 10 are now visibly distinct
    // grays (where the NTSC //gs-corrected default merges them onto a
    // single neutral). The "Chat Mauve trademark" actually shows up here,
    // not in HGR.
    const bool useChatMauve = (hiResMode == HiResMode::ChatMauveRGB) && (chatMauve != nullptr);
    const uint32_t* palette = useChatMauve ? kChatMauveLoResPalette : kLoResPalette;

    // Each lo-res row corresponds to half a text row (4 scanlines).
    for (int blockRow = firstRow; blockRow < lastRow; ++blockRow) {
        const int textRow = blockRow / 2;
        const bool upperHalf = (blockRow % 2 == 0);
        const uint16_t rowAddr = textRowAddress(textRow, state.page2);
        for (int col = 0; col < 40; ++col) {
            const uint8_t b = ram[rowAddr + col];
            const uint8_t nibble = upperHalf ? (b & 0x0F) : (b >> 4);
            const uint32_t rgb = palette[nibble];
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
// Colour decode follows MAME's `apple2video.cpp` (PR #10773 by benrg) —
// the gold-standard algorithm calibrated against real Apple II hardware.
// Three building blocks:
//
//   1. **Bit doubler.** Each of the 7 visible HGR bits is duplicated to
//      give a 14-bit word per byte (40 bytes × 14 = 560 sub-pixels per
//      scanline at the master 14.32 MHz cadence).
//   2. **Half-dot delay (MSB).** When a byte's bit 7 is set, the entire
//      14-bit word is shifted left by 1 sub-pixel, with the *top* bit
//      of the previous byte's word feeding bit 0. That single-cell
//      shift is the 74LS74 flip-flop delay (~70 ns / 90° chroma phase)
//      that real silicon implements. Because the delay lives in the
//      stream, fringing at MSB-toggle byte boundaries falls out for
//      free.
//   3. **7-bit sliding window + 4-phase rotation.** A 7-bit window walks
//      the 14-bit-per-byte stream with 3 bits of left context. For each
//      sub-pixel position the window indexes a 128-entry static LUT
//      (verbatim from MAME); the LUT entry is a byte that packs four
//      4-bit "lo-res palette index" candidates, one per NTSC phase.
//      `rotl4b(byte, x)` extracts the candidate matching the current
//      absolute sub-pixel x mod 4. The 4-bit result is the lo-res
//      palette index — the artefact colour drops out of the same
//      16-colour table that drives `renderLoRes()`.
//
// Output is at 560 sub-pixels per scanline; we average pairs into 280
// framebuffer pixels (the chroma-bandwidth-limited downsample real
// CRTs perform optically).
//
// Monochrome paths reuse the doubled bit stream but skip the LUT and
// rotation — luminance only, multiplied by a phosphor tint. Persistence
// for amber rides on a per-pixel history × decay buffer.
//
// Convention: bit 0 of an HGR byte is the LEFTMOST pixel, bit 6 the
// RIGHTMOST, bit 7 the per-byte half-dot delay flag.

namespace {

constexpr int kStreamLen = 560;   // 280 visible color clocks × 2 sub-pixels

// Bit doubler. `kBitDoubler[i]` is the 14-bit word obtained by replacing
// each of the 7 low bits of i with a doubled (b, b) pair.
constexpr std::array<uint16_t, 128> makeBitDoubler()
{
    std::array<uint16_t, 128> t{};
    for (unsigned i = 1; i < 128; ++i)
        t[i] = static_cast<uint16_t>(t[i >> 1] * 4 + (i & 1) * 3);
    return t;
}
constexpr std::array<uint16_t, 128> kBitDoubler = makeBitDoubler();

// Verbatim from MAME `apple2video.cpp` `artifact_color_lut[0]` (the
// composite/NTSC variant — the second LUT in the [2][128] table is for
// the slightly differently-tuned RGB monitor mode and lives in the same
// file should we ever need it). Each byte packs four 4-bit lo-res
// palette indices, one per NTSC sub-cycle phase; `rotl4b` selects which.
constexpr uint8_t kArtifactColorLut[128] = {
    0x00,0x00,0x00,0x00,0x88,0x00,0x00,0x00,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x22,0x22,0x66,0x66,0xaa,0xaa,0xee,0xee,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x55,0x55,0x55,0x55,0x99,0x99,0xdd,0xff,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0x77,0x77,0x77,0x77,0xff,0xff,0xff,0xff,
    0x00,0x00,0x00,0x00,0x88,0x88,0x88,0x88,0x11,0x11,0x55,0x11,0x99,0x99,0xdd,0xff,
    0x00,0x22,0x66,0x66,0xaa,0xaa,0xaa,0xaa,0x33,0x33,0x33,0x33,0xbb,0xbb,0xff,0xff,
    0x00,0x00,0x44,0x44,0xcc,0xcc,0xcc,0xcc,0x11,0x11,0x55,0x55,0x99,0x99,0xdd,0xdd,
    0x00,0x22,0x66,0x66,0xee,0xaa,0xee,0xee,0xff,0xff,0xff,0x77,0xff,0xff,0xff,0xff,
};

// `rotl4b(n, count)` — extract the 4-bit nibble of `n` at logical
// position `count` (mod 4). Maps to the NTSC phase rotation MAME uses.
constexpr unsigned rotl4b(unsigned n, unsigned count)
{
    return (n >> ((-static_cast<int>(count)) & 3)) & 0x0fu;
}

// Decode 40 HGR bytes into a 40-element array of 14-bit doubled words,
// applying the half-dot delay when the source byte's MSB is set.
void buildHgrWordRow(const uint8_t* ram, uint16_t rowAddr,
                     uint16_t (&words)[40])
{
    unsigned last_output_bit = 0;
    for (int col = 0; col < 40; ++col) {
        const uint8_t b = ram[rowAddr + col];
        uint16_t word = kBitDoubler[b & 0x7Fu];
        if (b & 0x80u) {
            word = static_cast<uint16_t>(((word << 1) | last_output_bit) & 0x3FFFu);
        }
        words[col] = word;
        last_output_bit = (word >> 13) & 1u;
    }
}

// Build the 560-sub-pixel raw bit stream — one entry per sub-pixel,
// 0/1. Used by the monochrome paths (which don't need the windowed
// LUT). Equivalent to laying buildHgrWordRow's output end-to-end.
void buildBitStream(const uint8_t* ram, uint16_t rowAddr,
                    uint8_t (&stream)[kStreamLen])
{
    uint16_t words[40];
    buildHgrWordRow(ram, rowAddr, words);
    int out = 0;
    for (int col = 0; col < 40; ++col) {
        const uint16_t w = words[col];
        for (int b = 0; b < 14; ++b) {
            stream[out++] = static_cast<uint8_t>((w >> b) & 1u);
        }
    }
}

inline uint32_t avgRgb(uint32_t a, uint32_t b)
{
    const uint32_t r = ((a & 0xFFu) + (b & 0xFFu)) >> 1;
    const uint32_t g = (((a >> 8)  & 0xFFu) + ((b >> 8)  & 0xFFu)) >> 1;
    const uint32_t bl = (((a >> 16) & 0xFFu) + ((b >> 16) & 0xFFu)) >> 1;
    return (uint32_t(0xFF) << 24) | (bl << 16) | (g << 8) | r;
}

// Phosphor table for the monochrome modes. RGB is the fully-lit colour
// (luminance 1.0); decay is the per-frame multiplier on the history
// buffer (0.0 = no afterglow, 1.0 = freeze). Indexed by the HiResMode
// enum's integer value — slots that aren't monochrome (ColorNTSC,
// ChatMauveRGB) are placeholders that are never actually read.
struct Phosphor { uint8_t r, g, b; float decay; };
constexpr Phosphor kPhosphors[] = {
    { 0xFF, 0xFF, 0xFF, 0.00f }, // ColorNTSC    — placeholder (color path)
    { 0xFF, 0xFF, 0xFF, 0.00f }, // ChatMauveRGB — placeholder (color path)
    { 0xFF, 0xFF, 0xFF, 0.00f }, // MonoWhite
    { 0x33, 0xFF, 0x33, 0.85f }, // MonoGreen P31 (CIE x=0.280, y=0.595)
    { 0xFF, 0xB0, 0x00, 0.96f }, // MonoAmber (long persistence)
};

// Le Chat Mauve / Video-7 AppleColor RGB — 6-color HGR palette, applied
// per-pixel-pair with the byte's MSB selecting the bank. ABGR-in-uint32
// (R lowest byte) to match `kLoResPalette`.
//
// On STANDARD HGR (no DHGR), real Chat Mauve / Video-7 hardware sniffs
// the digital pre-modulation video signal at the slot connector and
// decodes it directly into RGB — bypassing the NTSC modulator entirely.
// What this means concretely (cf. AppleWin RGBMonitor.cpp PR #837 and
// *Le Chat Mauve* manual):
//
//   - The MSB ("high bit") of each byte is a **palette bank flag**, NOT
//     a half-dot delay. Real Chat Mauve does NOT shift pixels by ½ dot;
//     that shift is purely an NTSC artefact mechanism Wozniak co-opted
//     to access the orange/blue half of the wheel via composite phase.
//   - A clean RGB decoder doesn't double bits to 14 sub-pixels per byte
//     either — it processes the raw 7-bit-per-byte stream directly.
//   - Color comes from **pairs of consecutive bits**. With the byte's
//     MSB selecting the bank:
//          MSB=0:  00→black  01→VIOLET  10→GREEN   11→white
//          MSB=1:  00→black  01→BLUE    10→ORANGE  11→white
//     6 distinct colours total — same as NTSC HGR on a colour TV, but
//     emitted cleanly with no inter-byte fringing and crisp edges
//     because the MSB transition is instantaneous, not phase-encoded.
//
// The 16-color palette with two distinct grays (the famous Chat Mauve /
// French Touch trademark) ONLY applies in DHGR mode (4-bit windows over
// the aux+main interleaved stream). DHGR isn't modelled here — it would
// require an aux RAM model first (see TODO.md §12). On standard HGR the
// $5 / $A bit patterns that NTSC reads as "gray" actually decode to
// VIOLET / GREEN (or BLUE / ORANGE with MSB=1) under Chat Mauve too —
// they're never grays on plain HGR.
//
// Indexing convention: kChatMauveHGR[msb][bit_pair].
constexpr std::array<std::array<uint32_t, 4>, 2> kChatMauveHGR = {{
    // MSB = 0 → "violet bank"
    { 0xFF000000,   //  00  black
      0xFFDD22DD,   //  01  violet  (purple)
      0xFF22DD11,   //  10  green
      0xFFFFFFFF }, //  11  white
    // MSB = 1 → "blue bank"
    { 0xFF000000,   //  00  black
      0xFFFF2222,   //  01  blue    (medium blue)
      0xFF1188FF,   //  10  orange
      0xFFFFFFFF }, //  11  white
}};

} // namespace

void Apple2Display::renderHiRes(Memory& mem, int firstScanline, int lastScanline)
{
    const auto state = mem.getDisplayState();
    const uint8_t* ram = mem.data();

    std::array<uint32_t, kWidth> raw;

    // Effective mode: ChatMauveRGB without a plugged card silently falls
    // back to NTSC (matches a real Apple II that's been pulled out of its
    // RGB adapter — the composite signal is still on the wire).
    const HiResMode effMode =
        (hiResMode == HiResMode::ChatMauveRGB && !chatMauve)
            ? HiResMode::ColorNTSC
            : hiResMode;

    // Le Chat Mauve / Video-7 RGB card. Decoded DIRECTLY from the raw
    // byte stream — bypassing every NTSC-specific transformation in this
    // file: no `kArtifactColorLut` 7-bit window, no `buildHgrWordRow`
    // bit doubler, no MSB half-dot delay. Real Chat Mauve hardware taps
    // the digital video data line at the slot and performs its own
    // hardware decode in TTL; the only Apple II video signal it consumes
    // is the raw 7-bit-per-byte serial stream, which we reproduce here
    // by walking `ram[rowAddr+col]` and shifting out the low 7 bits.
    //
    // Algorithm (see comment above kChatMauveHGR for the rationale):
    //   - Each byte exposes 7 visible pixels (bits 0..6, bit 0 = leftmost).
    //   - Bit 7 ("MSB") is a per-byte palette bank flag.
    //   - Pixels are decoded in PAIRS aligned to the line origin (pair
    //     boundaries on even pixel positions: (0,1), (2,3), …, (278,279)
    //     — 140 pairs at 280-pixel resolution).
    //   - Each pair → one palette entry, painted onto BOTH pixels of the
    //     pair (140-color-clocks × 2 = 280 pixels of identical colour).
    //   - For pairs that straddle a byte boundary, the MSB of the byte
    //     containing the LEFT pixel of the pair selects the bank.
    //
    // FIFO mode (BW560 / Mixed / Chunky / COL140) sub-variants:
    //   - BW560     → strict B&W: each pixel is just its raw bit, no
    //                 colour decoding.
    //   - everything else → the 6-colour HGR Chat Mauve palette above.
    //
    // The truly distinguishing visual against NTSC is NOT 16 vs 6 colours
    // (we're on standard HGR, not DHGR) — it's the absence of fringing
    // at byte boundaries when the MSB toggles, and the absence of phase
    // ambiguity in alternating bit patterns. Edges are sharp.
    if (effMode == HiResMode::ChatMauveRGB) {
        using Mode = LeChatMauveCard::RenderMode;
        const Mode mode = chatMauve->currentMode();
        const bool monochrome = (mode == Mode::BW560);

        for (int y = firstScanline; y < lastScanline; ++y) {
            const uint16_t rowAddr = hgrRowAddress(y, state.page2);

            // Lay out the 280 raw pixels (low 7 bits per byte, no doubling).
            uint8_t  pixels[kWidth];
            uint8_t  msbHigh[40];
            for (int col = 0; col < 40; ++col) {
                const uint8_t b = ram[rowAddr + col];
                msbHigh[col] = (b >> 7) & 1u;
                pixels[col * 7 + 0] = (b >> 0) & 1u;
                pixels[col * 7 + 1] = (b >> 1) & 1u;
                pixels[col * 7 + 2] = (b >> 2) & 1u;
                pixels[col * 7 + 3] = (b >> 3) & 1u;
                pixels[col * 7 + 4] = (b >> 4) & 1u;
                pixels[col * 7 + 5] = (b >> 5) & 1u;
                pixels[col * 7 + 6] = (b >> 6) & 1u;
            }

            if (monochrome) {
                for (int x = 0; x < kWidth; ++x) {
                    raw[x] = pixels[x] ? 0xFFFFFFFFu : 0xFF000000u;
                }
            } else {
                for (int p = 0; p < kWidth; p += 2) {
                    // Left pixel of the pair determines which byte's MSB
                    // we honour; that byte is at index p / 7. Two-bit
                    // code: bit at p is LSB, bit at p+1 is bit 1.
                    const unsigned code = pixels[p] | (pixels[p + 1] << 1);
                    const int      byteIdx = p / 7;
                    const uint32_t rgb = kChatMauveHGR[msbHigh[byteIdx]][code];
                    raw[p]     = rgb;
                    raw[p + 1] = rgb;
                }
            }

            uint32_t* outRow = frame.data() + static_cast<size_t>(y) * kWidth;
            std::memcpy(outRow, raw.data(), sizeof(raw));
        }
        return;
    }

    if (effMode == HiResMode::ColorNTSC) {
        // MAME-style 7-bit sliding-window decode. ContextBits = 3 leaves
        // the centre sub-pixel at bit 3 of the window, with 3 bits of
        // left context (the tail of the previous byte) and 3 bits of
        // right context (the head of the next byte) on either side.
        constexpr int kContextBits = 3;
        uint16_t words[40];
        std::array<uint32_t, kStreamLen> subPixels;

        for (int y = firstScanline; y < lastScanline; ++y) {
            const uint16_t rowAddr = hgrRowAddress(y, state.page2);
            buildHgrWordRow(ram, rowAddr, words);

            // Scanline's 560 sub-pixels via incremental window. `w`
            // accumulates up to (3 + 14 + 14) = 31 bits — fits in a
            // uint32_t. Each iteration consumes one bit (`>>= 1`).
            uint32_t w = static_cast<uint32_t>(words[0]) << kContextBits;
            for (int col = 0; col < 40; ++col) {
                if (col + 1 < 40) {
                    w |= static_cast<uint32_t>(words[col + 1])
                         << (14 + kContextBits);
                }
                for (int b = 0; b < 14; ++b) {
                    const int absX = col * 14 + b;
                    const uint8_t lutEntry = kArtifactColorLut[w & 0x7Fu];
                    const unsigned loresIdx = rotl4b(lutEntry, static_cast<unsigned>(absX));
                    subPixels[absX] = kLoResPalette[loresIdx];
                    w >>= 1;
                }
            }

            // Downsample 560 sub-pixels → 280 framebuffer pixels by
            // pair averaging. This is the optical chroma-bandwidth-limit
            // a real CRT applies — without it, the 14 MHz bit pattern
            // would alias against the 7 MHz pixel grid.
            for (int x = 0; x < kWidth; ++x) {
                raw[x] = avgRgb(subPixels[2 * x], subPixels[2 * x + 1]);
            }

            uint32_t* outRow = frame.data() + static_cast<size_t>(y) * kWidth;
            std::memcpy(outRow, raw.data(), sizeof(raw));
        }
        return;
    }

    // (See bottom of file for IIe 80-col text helpers.)

    // Monochrome path. The bit stream is sampled at twice the visible
    // pixel rate — averaging adjacent sub-pixels gives the soft
    // horizontal anti-aliased luminance a real CRT's chroma-bandwidth
    // limit produces. Persistence is per-pixel max(target, prev × decay):
    // mimics the additive re-excitation + passive fade of phosphor
    // chemistry. Switching modes clears the buffer (see setHiResMode).
    uint8_t stream[kStreamLen];
    const Phosphor& phos = kPhosphors[static_cast<int>(effMode)];
    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, state.page2);
        buildBitStream(ram, rowAddr, stream);

        uint8_t* histRow = persistenceL.data() + static_cast<size_t>(y) * kWidth;
        for (int x = 0; x < kWidth; ++x) {
            const int sub = x * 2;
            const int lit = stream[sub] + stream[sub + 1];   // 0..2
            const int target = (lit * 255) / 2;
            const int prev   = static_cast<int>(static_cast<float>(histRow[x]) * phos.decay);
            const int merged = std::max(target, prev);
            histRow[x] = static_cast<uint8_t>(merged);

            const uint32_t r = (static_cast<uint32_t>(phos.r) * merged + 127) / 255;
            const uint32_t g = (static_cast<uint32_t>(phos.g) * merged + 127) / 255;
            const uint32_t b = (static_cast<uint32_t>(phos.b) * merged + 127) / 255;
            raw[x] = (uint32_t(0xFF) << 24) | (b << 16) | (g << 8) | r;
        }

        uint32_t* outRow = frame.data() + static_cast<size_t>(y) * kWidth;
        std::memcpy(outRow, raw.data(), sizeof(raw));
    }
}

// ─── IIe 80-column text ──────────────────────────────────────────────────
//
// On a IIe with 80COL on, the screen is 560 native horizontal pixels:
// 80 character cells × 7 px each. Aux RAM holds the EVEN columns (0,2,…)
// and main RAM holds the ODD columns (1,3,…). The display reads aux byte
// at the same logical address as the main byte — there's only one text
// page, just split across two banks. PAGE2 still selects between page 1
// and page 2 unless 80STORE is on (in which case writes to text page 1
// route to aux per the memory dispatcher; the display keeps reading from
// page 1 because only 80STORE+PAGE2 swaps banks at the memory layer, and
// the read here uses page 1 either way).
//
// The 4 KB IIe character ROM doubles as the alternate-character source:
// ALTCHAR=on selects the second 2 KB bank where flashing inverse becomes
// mousetext + non-flashing inverse. When the user has not loaded a real
// charset ROM (`roms/apple2_char.rom`), the built-in 5×7 fallback covers
// the printable range; ALTCHAR is then a no-op.

void Apple2Display::renderText80(Memory& mem, int firstRow, int lastRow,
                                 bool altCharSet)
{
    (void)altCharSet;  // built-in 5×7 fallback ignores ALTCHAR; charset ROM
                       // would consult banks here once one is loaded.
    const auto state = mem.getDisplayState();
    const uint8_t* main_ = mem.data();
    const uint8_t* aux_  = auxRam ? auxRam : mem.data();
    const bool flashPhase = (frameCounter / kFlashHalfPeriodFrames) & 1u;

    for (int row = firstRow; row < lastRow; ++row) {
        // 80STORE + PAGE2 already routes writes to aux at the memory
        // layer, so reading from page 1 is the right thing for both halves.
        const uint16_t rowAddr = textRowAddress(row, state.page2 && !state.eightyStore);
        for (int col = 0; col < 40; ++col) {
            // For each 40-byte text row, AUX byte renders the EVEN
            // 80-col cell (chars 0,2,4,…) at xCell0, MAIN byte the
            // ODD cell (chars 1,3,5,…) at xCell1.
            for (int half = 0; half < 2; ++half) {
                const uint8_t  src    = (half == 0) ? aux_[rowAddr + col]
                                                    : main_[rowAddr + col];
                const int      cellX  = col * 14 + half * 7;
                const int      cellY  = row * 8;
                uint8_t glyph[8];
                bool invert = false;
                bool flash  = false;
                resolveGlyph(src, glyph, invert, flash);
                if (flash && flashPhase) invert = !invert;
                for (int gy = 0; gy < 8; ++gy) {
                    const uint8_t row8 = glyph[gy];
                    for (int gx = 0; gx < 7; ++gx) {
                        bool lit = (gx >= 1 && gx <= 5)
                                && ((row8 >> (5 - gx)) & 1);
                        if (invert) lit = !lit;
                        const int px = cellX + gx;
                        const int py = cellY + gy;
                        frame80[py * kWidth80 + px] = lit ? 0xFFFFFFFFu : 0xFF000000u;
                    }
                }
            }
        }
    }
}

void Apple2Display::upscaleFrameToFrame80(int firstScanline, int lastScanline)
{
    // Pixel-double frame[] horizontally into frame80[] for the requested
    // scanline range. Each native 280-wide pixel becomes two 560-wide
    // pixels of identical colour. Used to bridge HGR (always rendered at
    // 280 wide) into the 560-wide frame80 buffer when mixed-mode 80-col
    // text is on at the bottom.
    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint32_t* in  = frame.data()    + static_cast<size_t>(y) * kWidth;
        uint32_t*       out = frame80.data()  + static_cast<size_t>(y) * kWidth80;
        for (int x = 0; x < kWidth; ++x) {
            const uint32_t p = in[x];
            out[x * 2 + 0] = p;
            out[x * 2 + 1] = p;
        }
    }
}

// ─── IIe Double Hi-Res (DHGR) ────────────────────────────────────────────
//
// DHGR doubles HGR's horizontal resolution by interleaving aux RAM with
// main RAM at the byte level. Per scanline (HGR address formula already
// resolves the base of the row):
//
//   for c in 0..39:
//     aux_byte  = aux  [base + c]   → 7 dots at columns [c*14 .. c*14+6]
//     main_byte = main [base + c]   → 7 dots at columns [c*14+7 .. c*14+13]
//     bit 0 of each byte is the leftmost dot in its half.
//
// Total: 40 byte-pairs × 14 dots = 560 dots per scanline.
//
// Color: walk a 4-bit window over the 560-dot stream in 4-dot strides.
// The 4 consecutive dots form a nibble (bit 0 = leftmost dot, bit 3 =
// rightmost), which is a standard lo-res palette index 0..15. Each
// 4-dot group is a single color cell painted onto all four dots.
//
// Monochrome: each dot is a luminance bit (1 = lit, 0 = off) tinted by
// the active phosphor. The persistence buffer is sized for 280 wide so
// we render mono DHGR without phosphor decay — the user can still pick
// a green / amber tint, just without the long-tail afterglow.

void Apple2Display::renderDhgr(Memory& mem, int firstScanline, int lastScanline)
{
    const auto state = mem.getDisplayState();
    const uint8_t* main_ = mem.data();
    const uint8_t* aux_  = auxRam ? auxRam : main_;

    const HiResMode m = hiResMode;
    const bool monochrome = (m == HiResMode::MonoWhite ||
                             m == HiResMode::MonoGreen ||
                             m == HiResMode::MonoAmber);

    // Phosphor lookup. The kPhosphors table lives in the anonymous
    // namespace above; kLoResPalette is a static class member.
    static const struct { uint8_t r, g, b; } kMonoTint[] = {
        { 0xFF, 0xFF, 0xFF }, // ColorNTSC    placeholder
        { 0xFF, 0xFF, 0xFF }, // ChatMauveRGB placeholder
        { 0xFF, 0xFF, 0xFF }, // MonoWhite
        { 0x33, 0xFF, 0x33 }, // MonoGreen
        { 0xFF, 0xB0, 0x00 }, // MonoAmber
    };
    const auto& tint = kMonoTint[static_cast<int>(m)];

    uint8_t dots[kWidth80];
    for (int y = firstScanline; y < lastScanline; ++y) {
        const uint16_t rowAddr = hgrRowAddress(y, state.page2);

        // Build the 560-dot stream from interleaved aux + main bytes.
        for (int c = 0; c < 40; ++c) {
            const uint8_t auxB  = aux_ [rowAddr + c];
            const uint8_t mainB = main_[rowAddr + c];
            const int base = c * 14;
            for (int i = 0; i < 7; ++i) {
                dots[base + i]     = static_cast<uint8_t>((auxB  >> i) & 1u);
                dots[base + 7 + i] = static_cast<uint8_t>((mainB >> i) & 1u);
            }
        }

        uint32_t* outRow = frame80.data() + static_cast<size_t>(y) * kWidth80;
        if (monochrome) {
            // 1 dot = 1 luminance bit, tint applied.
            for (int x = 0; x < kWidth80; ++x) {
                if (dots[x]) {
                    outRow[x] = (uint32_t(0xFF) << 24)
                              | (uint32_t(tint.b) << 16)
                              | (uint32_t(tint.g) << 8)
                              |  uint32_t(tint.r);
                } else {
                    outRow[x] = 0xFF000000u;
                }
            }
        } else {
            // 4-dot color cells indexed into the lo-res 16-color palette.
            for (int x = 0; x < kWidth80; x += 4) {
                const uint8_t nibble = static_cast<uint8_t>(
                      dots[x + 0]
                    | (dots[x + 1] << 1)
                    | (dots[x + 2] << 2)
                    | (dots[x + 3] << 3));
                const uint32_t rgb = kLoResPalette[nibble];
                outRow[x + 0] = rgb;
                outRow[x + 1] = rgb;
                outRow[x + 2] = rgb;
                outRow[x + 3] = rgb;
            }
        }
    }
}

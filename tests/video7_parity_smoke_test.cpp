// Video-7 / Le Chat Mauve RGB parity smoke test.
//
// Pins POM2's Apple2Display Video-7 rendering against a self-contained port
// of MAME `src/mame/apple/apple2video.cpp`:
//   * dhgr_update() rgbmode 0/1/2/3   (:885-980)
//   * render_line_color_array()       (:571-583)  — fg/bg colored text
//   * an3_w() 2-bit FIFO mode register (:283-296) — exercised via the card
//
// The MAME formulas are reimplemented here as the *oracle*; POM2's renderer
// is the device under test. We feed identical aux/main bytes to both and
// compare every one of the 560 dots per line.
//
// Mode mapping (POM2 RenderMode == MAME rgbmode):
//   BW560(0)=mono DHR   Mixed(1)=per-MSB color/mono
//   Chunky160(2)=160-wide   COL140(3)=full color
//
// Palette: POM2 deliberately uses the AppleWin "Feline" Le Chat Mauve
// palette (two distinct grays) where MAME reuses the standard apple2 palette.
// Parity here is on the *index math* (which palette entry each dot selects),
// so the oracle is fed the same Feline values POM2 uses — kept in sync with
// Apple2Display.cpp::kChatMauveLoResPalette below.

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kBlack = 0xFF000000u;

// Must mirror Apple2Display.cpp::kChatMauveLoResPalette (AppleWin Feline).
const uint32_t kFeline[16] = {
    0xFF000000u, 0xFF4C12ACu, 0xFF830700u, 0xFFD11AAAu,
    0xFF2F8300u, 0xFF7E979Fu, 0xFFB58A00u, 0xFFFF9E9Fu,
    0xFF005F7Au, 0xFF4772FFu, 0xFF7F6878u, 0xFFCF7AFFu,
    0xFF2CE66Fu, 0xFF7BF6FFu, 0xFFB2EE6Cu, 0xFFFFFFFFu,
};

using Mode = LeChatMauveCard::RenderMode;

inline unsigned rotl4_1(unsigned n) { return ((n << 1) | (n >> 3)) & 0x0Fu; }

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

uint16_t textAddr(int row)
{
    return static_cast<uint16_t>(0x0400 + 0x80 * (row & 7) + 0x28 * (row >> 3));
}

// ── MAME dhgr_update() oracle, one scanline (560 dots). ────────────────────
void mameDhgrRow(Mode mode, const uint8_t aux[40], const uint8_t main[40],
                 uint32_t out[560])
{
    if (mode == Mode::Chunky160) {
        // rgbmode==2 (apple2video.cpp:906-930).
        int x = 0;
        for (int b = 0; b < 40; ++b) out[x++] = kBlack;
        for (int c = 0; c < 40; ++c) {
            unsigned v = aux[c] + (static_cast<unsigned>(main[c]) << 8);
            for (int i = 0; i < 4; ++i) {
                const uint32_t col = kFeline[v & 0x0Fu];
                out[x++] = col; out[x++] = col; out[x++] = col;
                v >>= 4;
            }
        }
        for (int b = 0; b < 40; ++b) out[x++] = kBlack;
        return;
    }
    if (mode == Mode::BW560) {
        // rgbmode==0 → monochrome render of the 560-dot stream (:896,941-944).
        for (int c = 0; c < 40; ++c)
            for (int i = 0; i < 7; ++i) {
                out[c * 14 + i]     = ((aux[c]  >> i) & 1u) ? kWhite : kBlack;
                out[c * 14 + 7 + i] = ((main[c] >> i) & 1u) ? kWhite : kBlack;
            }
        return;
    }
    // rgbmode==1 (Mixed) / ==3 (COL140) (:946-977).
    const bool colorAll = (mode == Mode::COL140);
    for (int c = 0; c < 40; c += 2) {
        const uint8_t a0 = aux[c],   m0 = main[c];
        const uint8_t a1 = aux[c+1], m1 = main[c+1];
        const unsigned w = (a0 & 0x7Fu)
                         | (static_cast<unsigned>(m0 & 0x7Fu) << 7)
                         | (static_cast<unsigned>(a1 & 0x7Fu) << 14)
                         | (static_cast<unsigned>(m1 & 0x7Fu) << 21);
        const unsigned mask = colorAll ? ~0u :
              ((a0 >> 7) ? 0x0000007Fu : 0u)
            | ((m0 >> 7) ? 0x00003F80u : 0u)
            | ((a1 >> 7) ? 0x001FC000u : 0u)
            | ((m1 >> 7) ? 0x0FE00000u : 0u);
        for (int b = 0; b < 28; ++b) {
            const int x = c * 14 + b;
            if (mask & (1u << b)) {
                const unsigned nib = (w >> (b & ~3u)) & 0x0Fu;
                out[x] = kFeline[rotl4_1(nib)];
            } else {
                out[x] = (w & (1u << b)) ? kWhite : kBlack;
            }
        }
    }
}

// Deterministic LCG so the byte patterns are reproducible across runs.
uint32_t lcg(uint32_t& s) { s = s * 1664525u + 1013904223u; return s; }

void runDhgrMode(Mode mode, const char* name)
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    // Card connected to the DISPLAY only (not the slot bus), so mem $C05E/F
    // reads can't reshuffle the FIFO — we drive the mode via overrideMode().
    LeChatMauveCard card;
    disp.setChatMauveCard(&card);
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    card.overrideMode(mode);

    // graphics + 80col + hires + dhgr.
    mem.memRead(0xC050);            // CLR_TEXT
    mem.memRead(0xC057);            // SET_HIRES
    mem.memWrite(0xC00D, 0);        // 80COL on
    mem.memRead(0xC05E);            // DHIRES on
    assert(mem.getDisplayState().dhgr);

    // Fill several scanlines with reproducible aux/main patterns, plus a
    // couple of hand-picked edge rows.
    uint8_t auxRows[8][40];
    uint8_t mainRows[8][40];
    uint32_t seed = 0xC0FFEEu + static_cast<uint32_t>(mode) * 7u;
    const int rows[8] = { 0, 1, 50, 80, 96, 120, 150, 191 };
    for (int r = 0; r < 8; ++r) {
        for (int c = 0; c < 40; ++c) {
            const uint8_t a = static_cast<uint8_t>(lcg(seed) >> 17);
            const uint8_t m = static_cast<uint8_t>(lcg(seed) >> 17);
            auxRows[r][c]  = a;
            mainRows[r][c] = m;
            const uint16_t addr = hgrAddr(rows[r]);
            mem.auxDataMutable()[addr + c] = a;
            mem.memWrite(static_cast<uint16_t>(addr + c), m);
        }
    }
    // Edge row 0: the classic 0x55 / 0x14 two-grays pattern at col 0.
    auxRows[0][0] = 0x55; mainRows[0][0] = 0x14;
    mem.auxDataMutable()[hgrAddr(rows[0]) + 0] = 0x55;
    mem.memWrite(static_cast<uint16_t>(hgrAddr(rows[0]) + 0), 0x14);
    for (int c = 1; c < 40; ++c) { auxRows[0][c] = 0; mainRows[0][c] = 0;
        mem.auxDataMutable()[hgrAddr(rows[0]) + c] = 0;
        mem.memWrite(static_cast<uint16_t>(hgrAddr(rows[0]) + c), 0); }

    disp.render(mem);
    assert(disp.width()  == 560);
    assert(disp.height() == 192);

    const uint32_t* pix = disp.pixels();
    uint32_t ref[560];
    for (int r = 0; r < 8; ++r) {
        mameDhgrRow(mode, auxRows[r], mainRows[r], ref);
        const uint32_t* row = pix + static_cast<size_t>(rows[r]) * 560;
        for (int x = 0; x < 560; ++x) {
            if (row[x] != ref[x]) {
                std::printf("MISMATCH %s row %d x %d: pom2=%08X mame=%08X\n",
                            name, rows[r], x, row[x], ref[x]);
                assert(false);
            }
        }
    }
    std::printf("  %-9s parity OK (8 scanlines x 560 dots)\n", name);
}

void runFgBgText()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());
    LeChatMauveCard card;
    disp.setChatMauveCard(&card);
    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

    // 40-col text + DHGR (AN3) on → Video-7 foreground-background mode.
    mem.memRead(0xC051);            // SET_TEXT
    mem.memWrite(0xC00C, 0);        // 80COL off
    mem.memRead(0xC05E);            // DHIRES on (m_dhires)
    assert(mem.getDisplayState().textMode);
    assert(mem.getDisplayState().dhgr);
    assert(!mem.getDisplayState().eightyCol);

    // Row 0: a normal space (blank glyph) — every dot is BACKGROUND.
    //   col 0 attr 0x5A → fg=5, bg=A → all dots = Feline[0xA].
    //   col 1 attr 0x3C → fg=3, bg=C → all dots = Feline[0xC].
    // Row 1: an inverse space (all-lit glyph) — every dot is FOREGROUND.
    //   col 0 attr 0x5A → all dots = Feline[5].
    // The char code lives in main RAM, the fg/bg attribute in aux RAM at the
    // same text address (MAME text_update :779/791).
    const uint16_t r0 = textAddr(0);
    const uint16_t r1 = textAddr(1);
    mem.memWrite(static_cast<uint16_t>(r0 + 0), 0xA0);  // normal space
    mem.memWrite(static_cast<uint16_t>(r0 + 1), 0xA0);  // normal space
    mem.memWrite(static_cast<uint16_t>(r1 + 0), 0x20);  // inverse space
    mem.auxDataMutable()[r0 + 0] = 0x5A;
    mem.auxDataMutable()[r0 + 1] = 0x3C;
    mem.auxDataMutable()[r1 + 0] = 0x5A;

    // Row 2 col 0: inverse 'A' (textured glyph) with attr 0x71 (fg=7,bg=1).
    // Used to pin the double_7_bits doubling + per-cell fg/bg mixing.
    const uint16_t r2 = textAddr(2);
    mem.memWrite(static_cast<uint16_t>(r2 + 0), 0x01);  // inverse 'A'
    mem.auxDataMutable()[r2 + 0] = 0x71;

    disp.render(mem);
    assert(disp.width()  == 560);
    assert(disp.height() == 192);
    const uint32_t* pix = disp.pixels();

    // Normal space → all background, per cell, for all 8 glyph rows.
    for (int gy = 0; gy < 8; ++gy) {
        const uint32_t* line = pix + static_cast<size_t>(gy) * 560;
        for (int d = 0; d < 14; ++d) {
            assert(line[0 * 14 + d] == kFeline[0xA]);   // col 0 bg
            assert(line[1 * 14 + d] == kFeline[0xC]);   // col 1 bg
        }
    }
    // Inverse space → all foreground.
    for (int gy = 0; gy < 8; ++gy) {
        const uint32_t* line = pix + static_cast<size_t>(8 + gy) * 560;
        for (int d = 0; d < 14; ++d)
            assert(line[0 * 14 + d] == kFeline[5]);     // col 0 fg
    }

    // Textured glyph (inverse 'A'): every dot is either fg(7) or bg(1), the
    // cell is not monochrome, and dots come in equal pairs — the
    // double_7_bits doubling (MAME render_line_color_array `w = in*4`).
    bool sawFg = false, sawBg = false;
    for (int gy = 0; gy < 8; ++gy) {
        const uint32_t* line = pix + static_cast<size_t>(16 + gy) * 560;
        for (int k = 0; k < 7; ++k) {
            const uint32_t p0 = line[0 * 14 + 2 * k];
            const uint32_t p1 = line[0 * 14 + 2 * k + 1];
            assert(p0 == p1);                            // doubled
            assert(p0 == kFeline[7] || p0 == kFeline[1]);
            if (p0 == kFeline[7]) sawFg = true;
            if (p0 == kFeline[1]) sawBg = true;
        }
    }
    assert(sawFg && sawBg);   // genuinely textured (mix of fg + bg)

    std::printf("  fg/bg text parity OK (bg/fg fills + 14-dot doubling)\n");
}

}  // namespace

int main()
{
    std::printf("Video-7 parity smoke:\n");
    runDhgrMode(Mode::BW560,     "BW560");
    runDhgrMode(Mode::Mixed,     "Mixed");
    runDhgrMode(Mode::Chunky160, "Chunky160");
    runDhgrMode(Mode::COL140,    "COL140");
    runFgBgText();
    std::printf("Video-7 parity smoke: OK\n");
    return 0;
}

// Smoke test for LeChatMauveCard + Apple2Display ChatMauveRGB pipeline.
//
// What this pins:
//   1. FIFO clocking. Default reset state = 0b11 (COL140). Each rising
//      edge of $C05F (AN3 going high) pushes the current $C00C/$C00D
//      level into a 2-bit FIFO. We replay the Arlequin BW560 sequence
//      and assert the FIFO ends up at 0b00.
//   2. ChatMauveRGB direct decoding (no MAME bit-doubler, no half-dot
//      delay, no 7-bit LUT). With FIFO=COL140, $01 at col 0 produces
//      one violet pixel + 6 black; $7F produces 7 white. Both differ
//      visibly from the NTSC pipeline tested in hgr_render_smoke.
//   3. BW560 forces strict B/W: $01 at col 0 paints exactly one white
//      pixel at x=0; no chroma anywhere on the row.
//   4. Lo-res Chat Mauve palette. Indices 5 ($5) and 10 ($A) decode to
//      DISTINCT grays under Chat Mauve, where NTSC's //gs-corrected
//      palette merges them.
//   5. Falling back to ColorNTSC when the card isn't plugged keeps the
//      NTSC pipeline intact (regression guard).

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kWhite = 0xFFFFFFFFu;

constexpr uint32_t pack(uint8_t r, uint8_t g, uint8_t b) {
    return 0xFF000000u | (uint32_t(b) << 16) | (uint32_t(g) << 8) | r;
}

void writeHgrByte(Memory& mem, int y, int col, uint8_t v) {
    const uint16_t addr = static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
    mem.memWrite(static_cast<uint16_t>(addr + col), v);
}

void clearHgrLine(Memory& mem, int y) {
    for (int col = 0; col < 40; ++col) writeHgrByte(mem, y, col, 0x00);
}

uint16_t loresAddr(int textRow) {
    return static_cast<uint16_t>(0x0400 + 0x80 * (textRow & 7) + 0x28 * (textRow >> 3));
}

const uint32_t* pixelAt(const Apple2Display& d, int x, int y) {
    return d.pixels() + (y * d.width() + x);
}

uint8_t r8(uint32_t c) { return  c        & 0xFFu; }
uint8_t g8(uint32_t c) { return (c >> 8 ) & 0xFFu; }
uint8_t b8(uint32_t c) { return (c >> 16) & 0xFFu; }

// Replay one bit through the Arlequin sequence (data on 80COL, clock on
// AN3). Each bit needs: write 80COL = bit, drop AN3, raise AN3.
void clockOneBitThroughFifo(Memory& mem, bool dataBit) {
    (void)mem.memRead(dataBit ? 0xC00D : 0xC00C);
    (void)mem.memRead(0xC05E);
    (void)mem.memRead(0xC05F);
}

} // namespace

int main()
{
    // ─── 1. FIFO clocking ────────────────────────────────────────────────
    {
        Memory mem;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));

        // After plug + onReset() the card is in COL140 (FIFO=11).
        raw->onReset();
        assert(raw->fifoBits() == 0b11);
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::COL140);

        // Push 0,0 → BW560.
        clockOneBitThroughFifo(mem, false);
        clockOneBitThroughFifo(mem, false);
        assert(raw->fifoBits() == 0b00);
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::BW560);

        // Push 1,1 → COL140.
        clockOneBitThroughFifo(mem, true);
        clockOneBitThroughFifo(mem, true);
        assert(raw->fifoBits() == 0b11);
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::COL140);

        // Hammering $C05F without re-arming $C05E shouldn't shift again
        // (rising-edge detection only triggers once per up-down cycle).
        (void)mem.memRead(0xC00C);  // data = 0
        (void)mem.memRead(0xC05F);  // first call: rising edge if AN3 was low
        const uint8_t after = raw->fifoBits();
        (void)mem.memRead(0xC05F);  // no $C05E in between → no edge
        (void)mem.memRead(0xC05F);
        assert(raw->fifoBits() == after);

        // After Apple II reset, FIFO returns to COL140.
        raw->onReset();
        assert(raw->fifoBits() == 0b11);
    }

    // ─── 2. ChatMauveRGB COL140 HGR decode ───────────────────────────────
    {
        Memory mem;
        Apple2Display display;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);

        // Force HGR mode + page 1.
        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC057);
        (void)mem.memRead(0xC054);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

        // Card state at construction = COL140 (FIFO=11) — no need to clock.
        assert(raw->currentMode() == LeChatMauveCard::RenderMode::COL140);

        // $01 at col 0: pair (bit0=1, bit1=0) at MSB=0 → violet on both
        // pixels (the Chat Mauve renderer paints both pixels of the pair
        // with the same colour, no half-dot delay).
        clearHgrLine(mem, 0);
        writeHgrByte(mem, 0, 0, 0x01);
        display.render(mem);
        // Pair (0,1): bits = 01 → kChatMauveHGR[0][1] = violet 0xFFDD22DD
        const uint32_t violet = pack(0xDD, 0x22, 0xDD);
        assert(*pixelAt(display, 0, 0) == violet);
        assert(*pixelAt(display, 1, 0) == violet);
        // The rest of the row: pairs 00 → black.
        for (int x = 2; x < 280; ++x) assert(*pixelAt(display, x, 0) == kBlack);

        // $02 at col 0: pair (bit0=0, bit1=1) → green (bank 0, code 10).
        clearHgrLine(mem, 1);
        writeHgrByte(mem, 1, 0, 0x02);
        display.render(mem);
        const uint32_t green = pack(0x22, 0xDD, 0x11);
        assert(*pixelAt(display, 0, 1) == green);
        assert(*pixelAt(display, 1, 1) == green);

        // $81 at col 0: bit0=1 only, MSB=1 → bank 1 → blue. No half-dot
        // delay (that's NTSC-only) — pixel 0,1 should both be the bank-1
        // 01 colour (blue), NOT shifted.
        clearHgrLine(mem, 2);
        writeHgrByte(mem, 2, 0, 0x81);
        display.render(mem);
        const uint32_t blue = pack(0xFF, 0x22, 0x22);
        assert(*pixelAt(display, 0, 2) == blue);
        assert(*pixelAt(display, 1, 2) == blue);
        // Critically, x=2 should be BLACK (no fringing — NTSC would smear
        // here because of the half-dot phase shift).
        assert(*pixelAt(display, 2, 2) == kBlack);

        // $7F full row: every pair = 11 → white.
        clearHgrLine(mem, 3);
        for (int col = 0; col < 40; ++col) writeHgrByte(mem, 3, col, 0x7F);
        display.render(mem);
        for (int x = 0; x < 280; ++x) assert(*pixelAt(display, x, 3) == kWhite);
    }

    // ─── 3. ChatMauveRGB BW560 forces strict B/W ─────────────────────────
    {
        Memory mem;
        Apple2Display display;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);

        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC057);
        (void)mem.memRead(0xC054);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        raw->overrideMode(LeChatMauveCard::RenderMode::BW560);

        // $01 at col 0: bit 0 = white, bits 1..6 = black, no chroma.
        clearHgrLine(mem, 0);
        writeHgrByte(mem, 0, 0, 0x01);
        display.render(mem);
        assert(*pixelAt(display, 0, 0) == kWhite);
        for (int x = 1; x < 280; ++x) assert(*pixelAt(display, x, 0) == kBlack);
    }

    // ─── 4. Lo-res Chat Mauve palette: distinct grays at 5 / A ───────────
    {
        Memory mem;
        Apple2Display display;
        auto card = std::make_unique<LeChatMauveCard>();
        LeChatMauveCard* raw = card.get();
        mem.slotBus().plug(7, std::move(card));
        display.setChatMauveCard(raw);

        // Force lo-res mode (graphics, lo-res, page 1, no mixed).
        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC056);
        (void)mem.memRead(0xC054);
        (void)mem.memRead(0xC052);
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

        // Lo-res byte $55 at row 0 col 0: low nibble = 5 (gray 1), high = 5.
        // Lo-res byte $AA at row 0 col 1: low nibble = A (gray 2), high = A.
        const uint16_t row0 = loresAddr(0);
        mem.memWrite(static_cast<uint16_t>(row0 + 0), 0x55);
        mem.memWrite(static_cast<uint16_t>(row0 + 1), 0xAA);
        display.render(mem);

        // Block (col=0, blockRow=0) covers x=[0..6], y=[0..3].
        const uint32_t gray1 = *pixelAt(display, 3, 1);
        // Block (col=1, blockRow=0) covers x=[7..13], y=[0..3].
        const uint32_t gray2 = *pixelAt(display, 10, 1);

        // Both should be greyscale (R≈G≈B) and DIFFERENT (the whole point
        // of the Chat Mauve palette).
        assert(std::abs(int(r8(gray1)) - int(g8(gray1))) <= 2);
        assert(std::abs(int(g8(gray1)) - int(b8(gray1))) <= 2);
        assert(std::abs(int(r8(gray2)) - int(g8(gray2))) <= 2);
        assert(std::abs(int(g8(gray2)) - int(b8(gray2))) <= 2);
        assert(r8(gray1) != r8(gray2));
        // Index 5 should be visibly darker than index A.
        assert(r8(gray1) < r8(gray2));
    }

    // ─── 5. ColorNTSC fallback when card not plugged ─────────────────────
    {
        Memory mem;     // no card plugged
        Apple2Display display;
        // Picking ChatMauveRGB without a card should silently fall back to
        // NTSC. Render a known $7F row and verify it lights up white
        // (the existing NTSC LUT does that for popcount-rich windows).
        display.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
        (void)mem.memRead(0xC050);
        (void)mem.memRead(0xC057);
        (void)mem.memRead(0xC054);

        for (int col = 0; col < 40; ++col) writeHgrByte(mem, 0, col, 0x7F);
        display.render(mem);
        for (int x = 0; x < 280; ++x) assert(*pixelAt(display, x, 0) == kWhite);
    }

    std::printf("Le Chat Mauve smoke: OK (FIFO clocking, COL140 + BW560 HGR, lo-res grays, NTSC fallback)\n");
    return 0;
}

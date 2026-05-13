// Display persistence + Chat Mauve 560-wide smoke test.
//
// Pins two recent fixes that were marked 🟡 in TODO.md §4 (Display):
//
//   (1) DHGR mono afterglow buffer
//       Before: `persistenceL` was sized for HGR (280×192), so the DHGR
//       mono path wrote a 560-wide framebuffer against a 280-wide
//       history → no per-pixel decay, MonoAmber DHGR looked identical
//       to MonoWhite. Fix: parallel `persistenceL80` buffer (560×192).
//       This test renders an all-bytes-set frame in MonoAmber DHGR,
//       then an all-bytes-cleared frame, and asserts that bright pixels
//       remain (afterglow active) — and that switching modes drops the
//       afterglow on the next frame (history reset hook).
//
//   (2) HGR + Le Chat Mauve at native 560-dot resolution
//       Before: the Chat Mauve HGR branch of renderHiRes wrote into
//       `frame` at 280-wide; the framebuffer therefore claimed the
//       card's lower-resolution output, throwing away the very 14 MHz
//       fidelity the card exists to provide. Fix: new
//       `renderHiResChatMauve80` writes into `frame80`, dispatched from
//       `render()` before the legacy 280-wide path. This test confirms
//       `width()==560` when HGR + Chat Mauve mode is active and checks
//       a known dot pattern lands at the expected 560-dot positions.

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint16_t IIE_80COL_ON  = 0xC00D;
constexpr uint16_t CLR_TEXT      = 0xC050;
constexpr uint16_t SET_HIRES     = 0xC057;
constexpr uint16_t DHIRES_ON     = 0xC05E;
constexpr uint16_t DHIRES_OFF    = 0xC05F;

uint16_t hgrAddr(int y)
{
    return static_cast<uint16_t>(0x2000
        + 0x400 * (y & 7)
        + 0x80  * ((y >> 3) & 7)
        + 0x28  * (y >> 6));
}

uint8_t alpha(uint32_t p) { return static_cast<uint8_t>((p >> 24) & 0xFF); }
uint8_t blueOf (uint32_t p) { return static_cast<uint8_t>((p >> 16) & 0xFF); }
uint8_t greenOf(uint32_t p) { return static_cast<uint8_t>((p >> 8)  & 0xFF); }
uint8_t redOf  (uint32_t p) { return static_cast<uint8_t>(p & 0xFF); }

// Luminance proxy on the framebuffer's R+G+B intensity. The amber phosphor
// emits (R=0xFF, G=0xB0, B=0x00) at full lit; non-zero on at least one
// channel ⇒ a pixel is on or glowing.
int lum(uint32_t p)
{
    return static_cast<int>(redOf(p)) + greenOf(p) + blueOf(p);
}

// ─── Item 1: DHGR mono afterglow ─────────────────────────────────────────

void testDhgrMonoAfterglow()
{
    Memory mem;
    mem.setIIEMode(true);
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    // DHGR full-screen: 80COL + HIRES + DHGR.
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
    mem.memWrite(IIE_80COL_ON, 0);
    mem.memRead(DHIRES_ON);

    // MonoAmber — the only built-in HiResMode whose decay is non-zero
    // (0.96, see kPhosphors). MonoWhite/Green have decay=0 → no glow,
    // which is correct but not interesting for an afterglow test.
    disp.setHiResMode(Apple2Display::HiResMode::MonoAmber);

    // Frame 1: light every HGR byte (main + aux) so every output dot is
    // lit. With 7 lit bits per byte the framebuffer should be saturated.
    for (uint32_t a = 0x2000; a < 0x4000; ++a) {
        mem.memWrite(static_cast<uint16_t>(a), 0x7F);
        mem.auxDataMutable()[a] = 0x7F;
    }
    disp.render(mem);
    assert(disp.width()  == 560);
    assert(disp.height() == 192);
    {
        const uint32_t* pix = disp.pixels();
        // A row in the middle of the visible area, far from any edge.
        const uint32_t p = pix[100 * 560 + 300];
        assert(lum(p) > 200);   // saturated amber
    }

    // Frame 2: clear everything. Without the persistenceL80 buffer the
    // pixels would drop straight to black; with it, MonoAmber's
    // decay=0.96 keeps the dots glowing.
    for (uint32_t a = 0x2000; a < 0x4000; ++a) {
        mem.memWrite(static_cast<uint16_t>(a), 0x00);
        mem.auxDataMutable()[a] = 0x00;
    }
    disp.render(mem);
    {
        const uint32_t* pix = disp.pixels();
        const uint32_t p = pix[100 * 560 + 300];
        // History × decay ≈ 255 × 0.96 ≈ 244 luminance — way above the
        // "no afterglow at all" baseline (which would be 0).
        const int L = lum(p);
        assert(L > 100 && "DHGR MonoAmber afterglow buffer not retaining luminance");
        // Tint is amber → blue channel stays at 0.
        assert(blueOf(p) == 0);
        // Red dominates green for amber.
        assert(redOf(p) > greenOf(p));
    }

    // Setting the mode (to itself) is a no-op (early return in
    // setHiResMode); switching to a different mode resets BOTH history
    // buffers. After a mode-switch + clear-frame render, no afterglow
    // should survive.
    disp.setHiResMode(Apple2Display::HiResMode::MonoWhite);
    disp.render(mem);  // still all-zero RAM
    {
        const uint32_t* pix = disp.pixels();
        const uint32_t p = pix[100 * 560 + 300];
        // MonoWhite has decay=0, so even if persistenceL80 hadn't been
        // cleared, the per-pixel target=0 wins via max(target, prev*0).
        // Combined with the mode-switch reset this should be hard black.
        assert(p == 0xFF000000u && "Mode switch failed to clear DHGR afterglow buffer");
    }

    // Switching back to MonoAmber + clean clear-frame: no glow because
    // the buffer was reset above.
    disp.setHiResMode(Apple2Display::HiResMode::MonoAmber);
    disp.render(mem);
    {
        const uint32_t* pix = disp.pixels();
        const uint32_t p = pix[100 * 560 + 300];
        assert(p == 0xFF000000u);
    }

    std::puts("  dhgr_mono_afterglow: OK");
}

// ─── Item 2: HGR Chat Mauve at native 560 ────────────────────────────────

void testHgrChatMauve560()
{
    Memory mem;
    mem.setIIEMode(true);     // Chat Mauve also works on II+, but DHGR
                              // gating depends on iieMode; testing in
                              // IIe mode keeps the dispatch realistic.
    Apple2Display disp;
    disp.setAuxMemory(mem.auxData());

    LeChatMauveCard chatMauve;
    disp.setChatMauveCard(&chatMauve);

    // HGR, NOT DHGR — exactly the Chat Mauve HGR case the new dispatch
    // is meant to catch. We deliberately leave 80COL off so we go
    // through the non-IIe-80col branch of the new code path.
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
    mem.memRead(DHIRES_OFF);
    assert(!mem.getDisplayState().dhgr);

    disp.setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);

    // Pattern: lay down a single byte at the leftmost column of HGR row 0
    // with bits = 0b0010_0011 = $23. The Chat Mauve algorithm groups
    // pixels into pairs:
    //   pixel 0 = 1, pixel 1 = 1 → pair code 0b11 = 3
    //   pixel 2 = 0, pixel 3 = 0 → pair code 0b00 = 0 (black)
    //   pixel 4 = 0, pixel 5 = 1 → pair code 0b10 = 2
    //   pixel 6 = 0           (last pixel — odd, pair 6 picks up pixel 7
    //                          from next byte = 0 → pair code 0b00 = 0)
    // MSB of byte = 0, so kChatMauveHGR[0][...] table lookup applies.
    // We don't pin the exact palette entry — that's covered by the
    // existing renderHiRes Chat Mauve coverage. What this test pins is:
    //   * width() reports 560 (we routed through frame80)
    //   * each input pair occupies 4 contiguous output dots (the doubling)
    //   * the BLACK pair at pixels 2..3 → output dots 4..7 all equal
    {
        mem.memWrite(hgrAddr(0), 0x23);
    }
    disp.render(mem);
    assert(disp.width()  == 560 && "HGR Chat Mauve still rendering at 280 wide");
    assert(disp.height() == 192);

    const uint32_t* pix = disp.pixels();
    // Row 0, first 4 output dots = pair 0 (pixels 0+1 = 0b11) → some
    // non-black palette entry; all 4 dots identical (the doubling
    // contract).
    const uint32_t c0 = pix[0 * 560 + 0];
    assert(pix[0 * 560 + 1] == c0);
    assert(pix[0 * 560 + 2] == c0);
    assert(pix[0 * 560 + 3] == c0);
    assert(alpha(c0) == 0xFF);

    // Row 0, dots 4..7 = pair 1 (pixels 2+3 = 0b00) → kChatMauveHGR[0][0]
    // which the original palette table sets to black. Pin the doubling
    // there too.
    const uint32_t c1 = pix[0 * 560 + 4];
    assert(pix[0 * 560 + 5] == c1);
    assert(pix[0 * 560 + 6] == c1);
    assert(pix[0 * 560 + 7] == c1);
    assert(c1 == 0xFF000000u && "Chat Mauve BLACK pair didn't expand to 4 dots");

    // Row 0, dots 8..11 = pair 2 (pixels 4+5 = 0b10) → another palette
    // entry, distinct from c0 (since the pair codes differ) and from
    // black. Doubling contract again.
    const uint32_t c2 = pix[0 * 560 + 8];
    assert(pix[0 * 560 + 9]  == c2);
    assert(pix[0 * 560 + 10] == c2);
    assert(pix[0 * 560 + 11] == c2);
    assert(c2 != c0 && c2 != 0xFF000000u);

    // BW560 mode: each input dot → 2 identical output dots. Plant a
    // simple alternating pattern and check.
    chatMauve.overrideMode(LeChatMauveCard::RenderMode::BW560);
    {
        // 0xAA = 0b1010_1010 (low 7 bits = 0b010_1010): pixels are
        // 0,1,0,1,0,1,0 (bit 0 leftmost).
        mem.memWrite(hgrAddr(0), 0xAA);
    }
    disp.render(mem);
    assert(disp.width() == 560);
    {
        const uint32_t* px = disp.pixels();
        // pixel 0 = 0 → dots 0,1 = black; pixel 1 = 1 → dots 2,3 = white.
        assert(px[0]  == 0xFF000000u);
        assert(px[1]  == 0xFF000000u);
        assert(px[2]  == 0xFFFFFFFFu);
        assert(px[3]  == 0xFFFFFFFFu);
        assert(px[4]  == 0xFF000000u);
        assert(px[5]  == 0xFF000000u);
    }
    std::puts("  hgr_chat_mauve_560: OK");
}

// Plain HGR (no Chat Mauve card / not in Chat Mauve mode) must still
// render at 280-wide. The new dispatch must not accidentally upgrade
// every HGR frame to 560.
void testNonChatMauveHgrStays280()
{
    Memory mem;
    Apple2Display disp;
    mem.memRead(CLR_TEXT);
    mem.memRead(SET_HIRES);
    disp.setHiResMode(Apple2Display::HiResMode::ColorNTSC);
    disp.render(mem);
    assert(disp.width()  == 280 && "Plain HGR NTSC must still be 280-wide");
    assert(disp.height() == 192);
    std::puts("  hgr_ntsc_stays_280: OK");
}

}  // namespace

int main()
{
    testDhgrMonoAfterglow();
    testHgrChatMauve560();
    testNonChatMauveHgrStays280();
    std::puts("display_persistence_smoke_test: OK");
    return 0;
}

// Static-text frame-skip equivalence test.
//
// Apple2Display skips repainting a full-screen text frame when the video RAM,
// the DisplayState, the FLASH phase, the colour mode and the character ROM are
// all byte-identical to the frame already in the framebuffer (see TextFrameKey
// in Apple2Display.h). This pins the ONE property that makes that legal:
//
//     the framebuffer must be bit-identical to what a full repaint produces,
//     every frame, whatever the guest does.
//
// Rather than assert specific pixels, it runs TWO machines side by side over
// the same scripted sequence — one display allowed to skip, one forced to
// repaint every frame via invalidateTextFrameCache() — and compares the whole
// framebuffer after each frame. Any divergence, anywhere, fails.
//
// Two SEPARATE Memory instances, not one shared by two displays: render()
// consumes the video-event queue (`mem.takeVideoEvents()`), so a second
// display rendering the same Memory would see an empty queue and never take
// the beam-racing path at all — the test would pass while testing nothing.
// The script is deterministic, so driving two machines with identical calls
// keeps them in lockstep.
//
// The script deliberately walks the cases the skip must NOT swallow:
//   * plain static text                 (the case being optimised)
//   * a single byte changing            (must repaint)
//   * the FLASH phase flipping          (must repaint, and it is time-driven,
//                                        not RAM-driven, so it is the easiest
//                                        one to get wrong)
//   * page 1 <-> page 2 flips
//   * 80-column and ALTCHAR toggles
//   * TEXT -> graphics -> MIXED -> TEXT
//   * BEAM-RACED frames (mid-frame soft-switch writes), which must never skip
//   * host-side COLOUR-MODE changes, which no guest write can signal — only
//     the key's hiResModeId sees them, and the Le Chat Mauve card is installed
//     because its colour-TEXT path is the one text renderer whose pixels
//     actually depend on the colour mode (every other mode draws text
//     hard-coded white-on-black)
//
// Mutation-tested 2026-07-31: deleting the flash-phase, video-RAM,
// DisplayState, colour-mode or beam-raced-frame terms from the key each makes
// this test fail. Two terms survive deletion and are therefore DEFENSIVE, not
// load-bearing, today: the `mixedMode` exclusion (renderInternalBand's
// `if (state.textMode)` short-circuits before any mixed handling, in both the
// 40- and 80-column paths, so MIXED cannot alter a full-text frame) and the
// `iie` flag (a machine only changes IIe-ness across a profile switch, which
// rebuilds the display). The character-ROM term is untestable here — the
// harness loads no ROM, and ROMs are user-provided so no test may require one.
//
// and it runs the whole script under BOTH video standards, because the FLASH
// phase derives from the emulated frame index — cycleCounter / (65 *
// scanlinesPerFrame) — so PAL's 312-line frame advances it at a different rate
// than NTSC's 262-line one. A skip keyed on a frame counter that ignored the
// standard would drift on PAL only.

#include "Apple2Display.h"
#include "CpuClock.h"
#include "LeChatMauveCard.h"
#include "Memory.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

int failures = 0;

struct Machine {
    Memory          mem;
    Apple2Display   disp;
    // Fresh card, default state (COL140, colour text on). Present because the
    // Le Chat Mauve colour-TEXT path is the one text renderer whose output
    // depends on the host-side colour mode — see section 8.
    LeChatMauveCard chat;

    explicit Machine(VideoStandard vs)
    {
        mem.setIIEMode(true);
        mem.setVideoStandard(vs);
        disp.setAuxMemory(mem.auxData());
        disp.setChatMauveCard(&chat);
    }
};

// What the UI and the screenshot paths actually consume: the active buffer for
// this frame (frame or frame80), with any deferred demod completed. Geometry is
// appended so an 80-column mismatch also shows up as a difference.
std::vector<uint32_t> grab(Apple2Display& d)
{
    const uint32_t* p = d.pixels();
    const size_t    n = static_cast<size_t>(d.width()) * static_cast<size_t>(d.height());
    std::vector<uint32_t> out(p, p + n);
    out.push_back(static_cast<uint32_t>(d.width()));
    out.push_back(static_cast<uint32_t>(d.height()));
    return out;
}

void compare(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
             const char* what, int frame, const char* standard)
{
    if (a == b) return;
    size_t       first = 0, differing = 0;
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i]) { if (!differing) first = i; ++differing; }
    std::printf("FAIL [%s] %s @frame %d: %zu/%zu words differ (sizes %zu/%zu), "
                "first at %zu (skip=%08X full=%08X)\n",
                standard, what, frame, differing, n, a.size(), b.size(), first,
                first < n ? a[first] : 0u, first < n ? b[first] : 0u);
    ++failures;
}

void runScript(VideoStandard vs, const char* standard)
{
    Machine skip(vs);   // allowed to take the fast path
    Machine full(vs);   // forced to repaint every frame

    Machine* const both[2] = { &skip, &full };

    auto poke = [&](uint16_t addr, uint8_t v) {
        for (Machine* m : both) m->mem.writeRamUnchecked(addr, v);
    };
    auto sw = [&](uint16_t addr) {
        for (Machine* m : both) m->mem.memWrite(addr, 0);
    };
    auto adv = [&](int cycles) {
        for (Machine* m : both) m->mem.advanceCycles(cycles);
    };

    const auto& vt = pom2VideoTiming(vs);
    const int frameCycles = 65 * vt.scanlinesPerFrame;

    // Seed a text screen with plenty of FLASH-range bytes ($40-$7F), so a
    // mishandled flash phase cannot hide in an all-normal screen.
    for (uint16_t a = 0x0400; a < 0x0C00; ++a)
        poke(a, static_cast<uint8_t>((a & 0x40) ? (0x40 + (a & 0x3F))
                                                : (0xA0 + (a & 0x3F))));

    int f = 0;
    auto frame = [&](const char* what) {
        full.disp.invalidateTextFrameCache();     // reference: always repaints
        skip.disp.render(skip.mem);
        full.disp.render(full.mem);
        compare(grab(skip.disp), grab(full.disp), what, f, standard);
        adv(frameCycles);
        ++f;
    };

    // 1. Static text — the case the skip exists for. Well past one FLASH
    //    half-period so the phase flips underneath the cache several times.
    for (int i = 0; i < 40; ++i) frame("static text");

    // 2. One byte changes, in the middle of a screen the cache is holding.
    poke(0x0400 + 17, 0xC1);
    for (int i = 0; i < 3; ++i) frame("one byte changed");

    // 3. Page flip, then back.
    sw(0xC055); for (int i = 0; i < 3; ++i) frame("page2 on");
    sw(0xC054); for (int i = 0; i < 3; ++i) frame("page2 off");

    // 4. 80-column and ALTCHAR — these change both the geometry and the glyph
    //    source, and 80-column also pulls half the cells from aux RAM.
    for (uint16_t a = 0x0400; a < 0x0800; ++a)
        for (Machine* m : both) m->mem.auxDataMutable()[a] = static_cast<uint8_t>(0xC1 + (a & 0x1F));
    sw(0xC00D); for (int i = 0; i < 3; ++i) frame("80col on");
    sw(0xC00F); for (int i = 0; i < 3; ++i) frame("altchar on");
    sw(0xC00E); for (int i = 0; i < 3; ++i) frame("altchar off");
    sw(0xC00C); for (int i = 0; i < 3; ++i) frame("80col off");

    // 5. TEXT -> graphics -> MIXED -> TEXT. Only the text frames may skip;
    //    graphics writes phosphor persistence and must always repaint.
    for (uint16_t a = 0x2000; a < 0x4000; ++a)
        poke(a, static_cast<uint8_t>(a * 7));
    sw(0xC050);                                        // CLRTEXT -> graphics
    sw(0xC057);                                        // SETHIRES
    for (int i = 0; i < 4; ++i) frame("hires graphics");
    sw(0xC053); for (int i = 0; i < 4; ++i) frame("mixed");
    sw(0xC052);                                        // CLRMIXED
    sw(0xC051); for (int i = 0; i < 4; ++i) frame("back to text");   // SETTEXT

    // 6. BEAM RACING: soft-switch writes DURING the frame produce video events,
    //    so the frame is painted as bands with different DisplayStates. These
    //    must never take the fast path, and the band boundaries are computed
    //    from the standard's scanline count — the PAL run is what would catch a
    //    262-hardcoded mapping.
    int beamFrames = 0;
    for (int i = 0; i < 6; ++i) {
        const int third = frameCycles / 3;
        adv(third);
        sw(0xC050);                 // CLRTEXT -> graphics, mid-frame
        adv(third);
        sw(0xC051);                 // SETTEXT -> text, mid-frame
        adv(frameCycles - 2 * third);  // cross the frame boundary: the events
                                       // are only PUBLISHED to the display at
                                       // the boundary, so rendering before this
                                       // would silently see an empty log and
                                       // test nothing.
        // takeVideoEvents() is a non-destructive copy in published mode (it
        // only moves under the legacy synchronous bracket, which this test
        // never opens), so peeking here does not rob the displays.
        if (!skip.mem.takeVideoEvents().empty()) ++beamFrames;
        full.disp.invalidateTextFrameCache();
        skip.disp.render(skip.mem);
        full.disp.render(full.mem);
        compare(grab(skip.disp), grab(full.disp), "beam-raced split", f, standard);
        ++f;
    }
    if (beamFrames == 0) {
        std::printf("FAIL [%s] beam-racing section produced NO video events — "
                    "the split path was never exercised\n", standard);
        ++failures;
    }

    // 7. Back to a quiet static screen — the skip must re-engage cleanly after
    //    all of the above rather than latch onto a stale key.
    for (int i = 0; i < 10; ++i) frame("static text again");

    // 8. Colour-mode changes on an otherwise frozen text screen. This is a
    //    HOST-side change — no guest write, no soft switch, nothing in video
    //    RAM moves — so it is invisible to every other term of the key. Only
    //    hiResModeId catches it. All of these render text through the
    //    renderInternal path (the CPU demods deliberately keep full-screen
    //    text crisp), so the framebuffer is a pure function of the mode and
    //    the two displays stay comparable.
    static const Apple2Display::HiResMode kModes[] = {
        Apple2Display::HiResMode::ColorCompMedium,
        Apple2Display::HiResMode::ChatMauveRGB,
        Apple2Display::HiResMode::MonoGreen,
        Apple2Display::HiResMode::ColorComp4Bit,
        Apple2Display::HiResMode::MonoAmber,
        Apple2Display::HiResMode::ColorAppleWin,
        Apple2Display::HiResMode::MonoWhite,
        Apple2Display::HiResMode::ColorNTSC,
    };
    sw(0xC05E);   // AN3 -> DHGR on: arms the card's colour-TEXT path
    for (auto m : kModes) {
        for (Machine* mm : both) mm->disp.setHiResMode(m);
        for (int i = 0; i < 3; ++i) frame("colour-mode change");
    }
    sw(0xC05F);

    std::printf("[%s] %d frames compared\n", standard, f);
}

}  // namespace

int main()
{
    std::puts("=== display static-text frame-skip equivalence ===");
    runScript(VideoStandard::NTSC, "NTSC");
    runScript(VideoStandard::PAL,  "PAL");

    if (failures) {
        std::printf("display_dirty_skip_test: FAIL (%d)\n", failures);
        return 1;
    }
    std::puts("display_dirty_skip_test: OK (skip is bit-identical to full repaint)");
    return 0;
}

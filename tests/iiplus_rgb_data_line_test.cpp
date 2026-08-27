// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// II+ : $C00C/$C00D reach the RGB card and NOTHING else.
//
// The 2026-07-12 Chat Mauve review flagged an asymmetry for awareness: on a
// IIe only WRITES to $C00C/$C00D do anything (the IIe's 80COL switch is
// write-only — a read of $C000-$C00F returns the keyboard latch and never
// touches the IOU), while on a II+ POM2 acts on ANY access.
//
// Checked 2026-08-27: the asymmetry is deliberate and harmless, and this test
// exists so it stays that way rather than being "tidied" into a bug.
//
// A real II+ has no IOU and no 80COL signal at all. The Le Chat Mauve /
// Video-7 class of RGB card sniffs $C00C/$C00D as the DATA line of its 2-bit
// mode FIFO, clocked by AN3 at $C05E/$C05F. On a IIe that data line is the
// real 80COL switch; on a II+ there is nothing to sniff, so POM2 synthesises
// it from the bus access — which is the whole reason the II+ branch exists.
//
// Two things must hold, and the second is what makes the first safe:
//   1. the card sees the data line and clocks its FIFO;
//   2. `display.eightyCol` set on a II+ is INERT for rendering, because every
//      consumer in Apple2Display gates on `mem.isIIE()`.

#include "Apple2Display.h"
#include "LeChatMauveCard.h"
#include "M6502.h"
#include "Memory.h"

#include <cstdio>
#include <memory>

namespace {

int failures = 0;

void expect(bool cond, const char* what)
{
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

using RenderMode = LeChatMauveCard::RenderMode;

/// One bit of the Arlequin sequence: set the data line, then pulse AN3.
/// The card shifts on the RISING edge only, so the low half must come first.
void clockBit(Memory& mem, bool dataBit)
{
    mem.memWrite(dataBit ? 0xC00D : 0xC00C, 0);
    mem.memWrite(0xC05E, 0);   // AN3 low
    mem.memWrite(0xC05F, 0);   // AN3 high → shift
}

} // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);
    mem.clearRam();
    mem.setIIEMode(false);            // II+ : no IOU, no 80COL switch

    auto card = std::make_unique<LeChatMauveCard>(7);
    auto* rgb = card.get();
    mem.slotBus().plug(7, std::move(card));

    // ── 1. The card sees the data line ───────────────────────────────────
    mem.memWrite(0xC00D, 0);
    expect(rgb->eightyCol(), "II+: $C00D did not reach the RGB data line");
    mem.memWrite(0xC00C, 0);
    expect(!rgb->eightyCol(), "II+: $C00C did not reach the RGB data line");

    // A READ must reach it too. This is the asymmetry: on a IIe the same read
    // would be inert (keyboard latch), but a II+ has no switch to be
    // write-only about, so the card is driven by the bus access itself.
    (void)mem.memRead(0xC00D);
    expect(rgb->eightyCol(), "II+: a READ of $C00D did not reach the data line");

    // ── 2. The FIFO clocks, so the card is actually usable on a II+ ───────
    // Shift in 0b11 → COL140, the mode the Arlequin sequence selects.
    clockBit(mem, true);
    clockBit(mem, true);
    expect(rgb->currentMode() == RenderMode::COL140,
           "II+: two 1 bits did not clock the FIFO to COL140");

    // ── 3. …and none of it renders as 80 columns ─────────────────────────
    // `display.eightyCol` is set by the II+ branch, but every consumer in
    // Apple2Display gates on isIIE(). width() publishes which buffer is live
    // — 280 for the 40-column frame, 560 for the 80-column one — so this is
    // a real differential and not a restatement of the flag.
    expect(mem.getDisplayState().eightyCol,
           "II+: the 80COL flag was not set (the RGB data line needs it)");

    (void)mem.memRead(0xC051);        // TEXT on
    Apple2Display iiPlusDisplay;
    iiPlusDisplay.render(mem);
    expect(iiPlusDisplay.width() == Apple2Display::kWidth,
           "II+: 80COL leaked into the render — 560-wide buffer went live");

    // The same flag on a IIe DOES go 80 columns. Without this half the check
    // above would pass just as well if 80-column rendering were broken
    // outright, which is not what is being claimed.
    {
        Memory iie;
        M6502  iieCpu(&iie);
        iie.setCpu(&iieCpu);
        iie.clearRam();
        iie.setIIEMode(true);
        iie.memWrite(0xC00D, 0);       // 80COL on — a WRITE, as the IIe needs
        (void)iie.memRead(0xC051);     // TEXT on
        Apple2Display iieDisplay;
        iieDisplay.render(iie);
        expect(iieDisplay.width() == Apple2Display::kWidth80,
               "IIe: 80COL did not select the 560-wide buffer");
    }

    if (failures) {
        std::printf("iiplus_rgb_data_line FAILED (%d)\n", failures);
        return 1;
    }
    std::printf("iiplus_rgb_data_line OK\n");
    return 0;
}

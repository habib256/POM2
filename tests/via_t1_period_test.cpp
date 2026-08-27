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

// 6522 Timer 1 continuous-mode period — must be latch + 2, not latch + 3.
//
// Why this test exists
// --------------------
// T1 free-run is the Apple II demoscene's frame clock: arm it with one
// video frame's worth of cycles and every IRQ lands on the same beam
// position. The period is therefore not a detail — it is the whole point.
// One cycle too long and the sync DRIFTS a cycle per frame, which is
// inaudible in a music tick (the usual Mockingboard use) but destroys any
// beam-raced effect once the accumulated slip crosses a scanline.
//
// POM2 used to reload with `latch + 3`, borrowed from MAME's
// `TIMER1_VALUE + IFR_DELAY`. IFR_DELAY models the one-off underflow→IFR
// latency; it is not part of the recurring period, and folding it in
// stretched every frame.
//
// The contract, stated by French Touch's "MAD EFFECT" while computing its
// own latch (`Sources/main.a`, GPLv3, shipped in disks_5.4/demo/madef/):
//
//     ; define DELAY for INT1
//     ; PAL delay = 65*(192+70+50) = 20280
//     ; -2 (6522 takes 2 cycles to generate INT)
//     ; = 20278 = $4F36
//
// i.e. a latch of N yields a period of N + 2. The demo's 192-line drawing
// loop slid one cycle per frame under +3 until whole scanlines of the
// picture fell into VBL and were dropped by the renderer.

#include "Via6522.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

// Drive the VIA one cycle at a time and return the cycle count at which T1
// next fires. `advance()` reports the underflow directly, so this does not
// depend on IFR read/clear side effects.
int cyclesToFire(pom2::Via6522& via, int limit)
{
    for (int c = 1; c <= limit; ++c)
        if (via.advance(1)) return c;
    return -1;
}

}  // namespace

int main()
{
    constexpr uint16_t kLatch = 20278;          // $4F36 — MAD EFFECT's PAL frame
    constexpr int      kPeriod = kLatch + 2;    // 20280 = 65 * 312

    pom2::Via6522 via;
    // ACR bit 6 = T1 continuous, PB7 disabled — exactly what the demo writes
    // to $C40B (`LDA #%01000000 / STA $C40B`).
    via.write(pom2::Via6522::VIA_ACR, 0x40);
    via.write(pom2::Via6522::VIA_IER, 0xC0);   // enable T1 interrupt

    // Arm: low latch then high latch — the high write starts the countdown.
    via.write(pom2::Via6522::VIA_T1CL, kLatch & 0xFF);
    via.write(pom2::Via6522::VIA_T1CH, (kLatch >> 8) & 0xFF);

    // First interval after the arming write.
    const int first = cyclesToFire(via, kPeriod * 2);
    std::printf("first fire at %d cycles (latch %u)\n", first, kLatch);
    assert(first > 0 && "T1 never fired");

    // Every SUBSEQUENT interval is the free-run period and must be exact:
    // this is the one that accumulates.
    for (int i = 0; i < 8; ++i) {
        const int n = cyclesToFire(via, kPeriod * 2);
        std::printf("  reload %d: %d cycles\n", i, n);
        assert(n == kPeriod &&
               "T1 continuous period must be latch + 2 — a one-cycle error "
               "here drifts a whole frame per frame and slides beam-raced "
               "effects off the screen");
    }

    // A degenerate tiny latch must not spin the reload loop (the collapse
    // path) and must still respect latch + 2.
    pom2::Via6522 tiny;
    tiny.write(pom2::Via6522::VIA_ACR, 0x40);
    tiny.write(pom2::Via6522::VIA_IER, 0xC0);
    tiny.write(pom2::Via6522::VIA_T1CL, 4);
    tiny.write(pom2::Via6522::VIA_T1CH, 0);
    cyclesToFire(tiny, 64);
    const int small = cyclesToFire(tiny, 64);
    std::printf("tiny latch 4 → period %d\n", small);
    assert(small == 6 && "latch 4 must give a 6-cycle period");

    std::printf("via_t1_continuous_period OK\n");
    return 0;
}

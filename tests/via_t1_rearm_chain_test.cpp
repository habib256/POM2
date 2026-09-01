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

// The 6522 T1 counter READ-BACK line, pinned by French Touch's re-arm idiom.
//
// TRIBU (DIX anthology, GPLv3 sources in disks_5.4/demo/
// unreeeal_superhero_3_tribute/Sources/main.a) phase-locks FIVE chained T1
// IRQs per PAL frame: each handler reads the free-running counter
// (`ADC $C404 … STA $C405`, read→write 21 cycles) and re-arms with
// `TIMERn - IntL + readback`, where every TIMERn carries
// `DELAY = 24 ; value FOR APPLEWIN / REAL APPLE II`. On hardware the chain
// is drift-free: five links sum to exactly one PAL frame (20280 cycles).
//
// That release-tuned constant fixes the counter read-back line to
// `written + 1 - elapsed` — bias -1 on POM2's +2-biased internal counter,
// NOT MAME's -IFR_DELAY (-2). With -2 every link lost one cycle and the
// whole DIX raster screen crawled left one character cell every ~3 frames
// (measured -5 cycles/frame on the real disk).
//
// What this pins:
//   1. Armed read-back: arm V, elapse e < V → read = V + 1 - e.
//   2. Continuous line through the reload: latch L, d cycles after a
//      reload boundary → read = L - d (period stays latch+2 —
//      `via_t1_continuous_period` owns that pin).
//   3. One-shot: the line V + 1 - e continues THROUGH the underflow
//      (mod 65536) — no step at the fire.
//   4. The TRIBU chain itself: 5 links re-armed with the demo's exact
//      arithmetic and spacing sum to 20280 cycles per frame, drift-free
//      over 40 frames.

#include "Via6522.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

using pom2::Via6522;

namespace {

uint16_t readCounter(Via6522& v)
{
    const uint8_t lo = v.read(0x4);
    const uint8_t hi = v.read(0x5);
    return static_cast<uint16_t>(lo | (hi << 8));
}

void armT1(Via6522& v, uint16_t val)
{
    v.write(0x4, static_cast<uint8_t>(val & 0xFF));
    v.write(0x5, static_cast<uint8_t>(val >> 8));
}

} // namespace

int main()
{
    // 1. Armed read-back: V + 1 - e.
    {
        Via6522 v;
        v.write(0xB, 0x00);            // one-shot
        armT1(v, 0x1234);
        v.advance(100);
        assert(readCounter(v) == 0x1234 + 1 - 100);
    }

    // 2. Continuous: line L - d after a reload boundary; reload period L+2.
    {
        Via6522 v;
        v.write(0xB, 0x40);            // T1 continuous
        const uint16_t L = 1000;
        armT1(v, L);
        // First fire at L+3; the counter then sits on the reload line.
        v.advance(L + 3 + 40);         // d = 40 past the fire
        assert(readCounter(v) == L - 40);
        // One full period later, same phase.
        v.advance(static_cast<int>(L) + 2);
        assert(readCounter(v) == L - 40);
    }

    // 3. One-shot: the same line V + 1 - e continues through the underflow.
    {
        Via6522 v;
        v.write(0xB, 0x00);
        armT1(v, 500);
        v.advance(400);
        assert(readCounter(v) == 500 + 1 - 400);
        v.advance(200);                // e = 600, past the fire at 503
        assert(readCounter(v) ==
               static_cast<uint16_t>((500 + 1 - 600) & 0xFFFF));
    }

    // 4. The TRIBU chain. Five links; per link the handler (entered a fixed
    //    latency after the IRQ) reads the counter and re-arms with
    //    TIMERn - IntL + read — the demo's exact 16-bit arithmetic, with
    //    the demo's exact instruction spacing: read lo at t, latch-lo
    //    write at t+4, read hi at t+11, arm (T1C-H write) at t+21.
    {
        static const int kT[5] = {
            8 * 8 * 65 - 24,           // INT_ROUT2 arms TIMER2
            3 * 8 * 65 - 24,
            11 * 8 * 65 - 24,
            1 * 8 * 65 - 24,
            (120 + 8) * 65 - 24,       // INT_ROUT1 wraps to the frame top
        };
        constexpr int kFrame = 312 * 65;   // PAL
        constexpr int kIrqLatency = 12;    // IRQ 7 + STA save_A + entry

        Via6522 v;
        v.write(0xB, 0x40);            // TRIBU runs T1 continuous
        uint16_t intL = 20280 & 0xFFFF;
        armT1(v, intL);
        uint64_t now = 0;

        uint64_t fires[5 * 44];
        int nFires = 0;
        while (nFires < 5 * 44) {
            // Walk to the IRQ, cycle by cycle (test-grade, exact).
            while (!(v.read(0xD) & 0x40)) { v.advance(1); ++now; }
            fires[nFires++] = now;
            // Handler: latency, then the read/re-arm dance.
            v.advance(kIrqLatency); now += kIrqLatency;
            const int k = (nFires - 1) % 5;
            const uint16_t tmp = static_cast<uint16_t>(kT[k] - intL);
            const uint8_t rLo = v.read(0x4);          // ADC $C404 (clears IFR)
            v.advance(4); now += 4;
            v.write(0x4, static_cast<uint8_t>(tmp + rLo));  // STA $C404
            v.advance(7); now += 7;
            const uint8_t rHi = v.read(0x5);          // ADC $C405
            const uint16_t r = static_cast<uint16_t>(rLo | (rHi << 8));
            const uint16_t val = static_cast<uint16_t>(tmp + r);
            v.advance(10); now += 10;
            v.write(0x5, static_cast<uint8_t>(val >> 8));   // STA $C405: arm
            intL = val;
        }
        // After a 2-frame warm-up, every frame is EXACTLY one PAL frame.
        for (int f = 2; f + 1 < 44; ++f) {
            const int64_t d = static_cast<int64_t>(fires[5 * (f + 1)]) -
                              static_cast<int64_t>(fires[5 * f]);
            if (d != kFrame) {
                std::fprintf(stderr, "frame %d: %lld cycles (want %d)\n",
                             f, static_cast<long long>(d), kFrame);
                assert(false && "TRIBU T1 chain must be drift-free");
            }
        }
    }

    std::printf("via_t1_rearm_chain: OK (read-back line V+1-e, TRIBU chain drift-free)\n");
    return 0;
}

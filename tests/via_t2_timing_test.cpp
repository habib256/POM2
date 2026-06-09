// 6522 Timer-2 one-shot underflow timing.
//
// MAME (`6522via.cpp:957-960`) schedules the T2 underflow IRQ at
// `TIMER2_VALUE + IFR_DELAY` cycles after the T2CH write, with IFR_DELAY = 3.
// A Timer-2-synced beam-racer (French Touch DIX: `T2 = 7512 − latency`, where
// the latency folds in this 6522 delay + the 6502 IRQ sequence) lands its
// mid-scanline effect on the intended byte only if T2 fires at exactly N+3 —
// POM2 used to fire at N+1 (counter crosses < 0), two bytes early.

#include "Via6522.h"

#include <cassert>
#include <cstdio>

using pom2::Via6522;

namespace {
constexpr uint8_t T2CL = 0x8, T2CH = 0x9, ACR = 0xB;
constexpr uint8_t IFR_T2 = 0x20;

// Cycles from the T2CH write until IFR.T2 latches, advancing one cycle at a
// time (so the test is independent of any chunk granularity).
int t2FireCycle(int n) {
    Via6522 via;
    via.write(ACR, 0x00);                 // T2 timed phase-2 mode (ACR bit5 = 0)
    via.write(T2CL, n & 0xFF);            // low latch
    via.write(T2CH, (n >> 8) & 0xFF);     // high latch → load counter + arm
    for (int c = 1; c <= n + 16; ++c) {
        via.advance(1);
        if (via.ifr & IFR_T2) return c;
    }
    return -1;
}
}  // namespace

int main()
{
    for (int n : {0, 1, 10, 100, 1000, 7479, 0x3FFF}) {
        const int c = t2FireCycle(n);
        std::printf("  T2=%d → IFR.T2 at cycle %d (MAME N+IFR_DELAY = %d)\n",
                    n, c, n + 3);
        assert(c == n + 3 && "T2 underflow IRQ timing != MAME TIMER2_VALUE+IFR_DELAY");
    }

    // One-shot: only ONE interrupt per T2CH write — after firing, further
    // counting must NOT re-raise IFR.T2 (MAME `m_t2_active = 0` after fire).
    {
        Via6522 via;
        via.write(ACR, 0x00);
        via.write(T2CL, 5);
        via.write(T2CH, 0);
        for (int c = 0; c < 8; ++c) via.advance(1);   // fire at cyc 8 (=5+3)
        assert(via.ifr & IFR_T2);
        via.read(T2CL);                                // T2CL read clears IFR.T2
        assert(!(via.ifr & IFR_T2));
        for (int c = 0; c < 0x20000; ++c) via.advance(1);  // counter wraps fully
        assert(!(via.ifr & IFR_T2) && "T2 one-shot re-fired (should arm once per T2CH)");
    }

    std::printf("OK via_t2_timing\n");
    return 0;
}

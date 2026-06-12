// 6522 timer + interrupt-flag timing (Via6522 unit level).
//
// T2: MAME (`6522via.cpp:957-960`) schedules the T2 underflow IRQ at
// `TIMER2_VALUE + IFR_DELAY` cycles after the T2CH write, with IFR_DELAY = 3.
// A Timer-2-synced beam-racer (French Touch DIX: `T2 = 7512 − latency`, where
// the latency folds in this 6522 delay + the 6502 IRQ sequence) lands its
// mid-scanline effect on the intended byte only if T2 fires at exactly N+3 —
// POM2 used to fire at N+1 (counter crosses < 0), two bytes early.
//
// T1: same constant — MAME schedules BOTH the first shot after a T1CH write
// (`6522via.cpp:927-943`, adjust at :941) and every continuous-mode reload
// (t1_tick, `:536-543`) at `TIMER1_VALUE + IFR_DELAY`. POM2 used to fire the
// first shot at N+1 while reloading at N+3 — the first IRQ of a music driver
// landed two cycles early relative to all subsequent ones.
//
// ORA / IFR.CA1: MAME's `CLR_PA_INT()` (`6522via.cpp:99`) runs on ANY access
// to register 1 (ORA) — read (:676) or write (:875) — clearing IFR.CA1 and,
// unless CA2 is in independent-interrupt mode (`CA2_IND_IRQ`, :51), IFR.CA2.
// Register $F (ORANH) is side-effect-free (:690-700, :888-896). Since
// setCa1NegativeEdge() can latch IFR.CA1 (Sound II SSI263 wiring), a driver
// acking via the standard ORA access must not see a stuck IRQ.

#include "Via6522.h"

#include <cassert>
#include <cstdio>

using pom2::Via6522;

namespace {
constexpr uint8_t ORA = 0x1, T1CL = 0x4, T1CH = 0x5, T1LL = 0x6;
constexpr uint8_t T2CL = 0x8, T2CH = 0x9, ACR = 0xB, PCR = 0xC, ORANH = 0xF;
constexpr uint8_t IFR_CA2 = 0x01;
constexpr uint8_t IFR_CA1 = 0x02;
constexpr uint8_t IFR_T1 = 0x40;
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

// Cycles from the T1CH write until IFR.T1 first latches (one-shot mode).
int t1FirstShotCycle(int n) {
    Via6522 via;
    via.write(ACR, 0x00);                 // T1 one-shot
    via.write(T1LL, n & 0xFF);            // latch low
    via.write(T1CH, (n >> 8) & 0xFF);     // latch high → load counter + arm
    for (int c = 1; c <= n + 16; ++c) {
        via.advance(1);
        if (via.ifr & IFR_T1) return c;
    }
    return -1;
}

void testT1FirstShotTiming()
{
    // First shot at exactly N+3, MAME `TIMER1_VALUE + IFR_DELAY`
    // (`6522via.cpp:941`, IFR_DELAY = 3 at :102).
    for (int n : {0, 1, 10, 100, 1000, 7479, 0x3FFF}) {
        const int c = t1FirstShotCycle(n);
        std::printf("  T1=%d → IFR.T1 at cycle %d (MAME N+IFR_DELAY = %d)\n",
                    n, c, n + 3);
        assert(c == n + 3 && "T1 first-shot timing != MAME TIMER1_VALUE+IFR_DELAY");
    }

    // Continuous mode: first shot at N+3, then every N+3 thereafter —
    // the same constant for shot and reload (MAME t1_tick :536-543).
    {
        constexpr int n = 100;
        Via6522 via;
        via.write(ACR, 0x40);             // T1 continuous
        via.write(T1LL, n);
        via.write(T1CH, 0);
        int fires = 0, last = 0;
        for (int c = 1; c <= 3 * (n + 3); ++c) {
            via.advance(1);
            if (via.ifr & IFR_T1) {
                ++fires;
                std::printf("  T1 continuous fire %d at cycle %d\n", fires, c);
                assert(c == fires * (n + 3) &&
                       "T1 continuous fire spacing != N+3");
                last = c;
                (void)via.read(T1CL);     // clear IFR.T1, keep counting
            }
        }
        (void)last;
        assert(fires == 3);
    }
    std::printf("OK t1 first-shot/reload timing (N+3)\n");
}

void testOraAccessClearsCa1()
{
    // PCR.0 = 0 (negative edge active — Sound II default). Latch CA1.
    Via6522 via;
    via.write(PCR, 0x00);
    via.ifr |= IFR_CA2;                   // pre-set CA2 too (no setter — POM2
                                          // never raises it; pin the clear)
    via.setCa1NegativeEdge();
    assert(via.ifr & IFR_CA1);

    // Reading ORANH ($F) must NOT clear (MAME 6522via.cpp:690-700).
    (void)via.read(ORANH);
    assert(via.ifr & IFR_CA1);
    assert(via.ifr & IFR_CA2);
    via.write(ORANH, 0x55);               // writes either (:888-896)
    assert(via.ifr & IFR_CA1);
    assert(via.ifr & IFR_CA2);

    // Reading ORA clears CA1 + CA2 (CA2 not independent, PCR=0) —
    // MAME CLR_PA_INT() at 6522via.cpp:676.
    (void)via.read(ORA);
    assert(!(via.ifr & IFR_CA1) && "ORA read must clear IFR.CA1");
    assert(!(via.ifr & IFR_CA2) && "ORA read must clear IFR.CA2 (dependent mode)");

    // Writing ORA clears too (MAME :875).
    via.setCa1NegativeEdge();
    via.ifr |= IFR_CA2;
    via.write(ORA, 0xAA);
    assert(!(via.ifr & IFR_CA1) && "ORA write must clear IFR.CA1");
    assert(!(via.ifr & IFR_CA2));

    // CA2 independent-interrupt mode: PCR[3:1] = 001 → (pcr & 0x0A) ==
    // 0x02 (MAME CA2_IND_IRQ, 6522via.cpp:51). ORA access still clears
    // CA1 but leaves CA2 latched.
    via.write(PCR, 0x02);
    via.setCa1NegativeEdge();
    via.ifr |= IFR_CA2;
    (void)via.read(ORA);
    assert(!(via.ifr & IFR_CA1));
    assert((via.ifr & IFR_CA2) && "independent CA2 must survive ORA access");

    std::printf("OK ora access clears CA1 (+CA2 unless independent)\n");
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

    testT1FirstShotTiming();
    testOraAccessClearsCa1();

    std::printf("OK via_t2_timing\n");
    return 0;
}

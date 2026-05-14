// Reproduces the canonical "4am / Total Replay / French Touch / Nox
// Archaist" Mockingboard detection routine and verifies it succeeds
// against POM2's lazy timer-sync.
//
// Source of the routine (deater/dos33fsprogs
// demos/demosplash2025/pt3_lib_mockingboard_detect.s):
//
//   mb_timer_check:
//       lda (MB_ADDR_L),Y   ; read 6522 T1CL at $Cn04
//       sta MB_VALUE        ; 3 cycles
//       lda (MB_ADDR_L),Y   ; read T1CL again
//       sec
//       sbc MB_VALUE        ; expect -8 (8 CPU cycles elapsed,
//                           ; T1 counted down by 8)
//       cmp #$F8
//       beq found
//
// The trap pre-fix: POM2 advanced slot peripherals in batched slices
// (at the end of each CPU run), so the two T1CL reads landed at the
// same VIA cycle count and produced delta = 0. The detection routine
// then concluded "no Mockingboard in this slot" and moved on. Every
// Total-Replay-derived driver (which is most modern Apple II games
// shipping Mockingboard support) was broken by this. With the lazy-
// sync added in `Mockingboard::syncToCpuCycle()`, each `slotRomRead`
// catches the VIAs up to `cpu->getCycleCountNow()` so the second read
// observes a counter 8 cycles further down.
//
// This test stages the *exact* CPU-driven sequence (not a synthetic
// MMIO call) so it pins the end-to-end behaviour, including how
// `getCycleCountNow()` interacts with mid-instruction `cycles`
// accumulation.

#include "Memory.h"
#include "M6502.h"
#include "Mockingboard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr int kMockSlot = 4;
constexpr uint16_t kMbAddrLZp = 0x10;   // MB_ADDR_L zero-page byte
constexpr uint16_t kMbAddrHZp = 0x11;   // MB_ADDR_H zero-page byte
constexpr uint16_t kMbValueZp = 0x12;   // MB_VALUE zero-page byte

int main_impl()
{
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);   // installs SlotBus IRQ router
    cpu.hardReset();

    auto card = std::make_unique<MockingboardCard>(kMockSlot);
    card->setCpu(&cpu); // lazy-sync back-channel for VIA timers
    MockingboardCard* cardPtr = card.get();
    mem.slotBus().plug(kMockSlot, std::move(card));

    // Anchor the lazy-sync clock — onReset() reads cpu->getCycleCountNow()
    // and the call below makes sure the card sees the current cycle
    // counter as "time zero" for delta math.
    cardPtr->onReset();

    // Set up zero-page indirection: MB_ADDR_L = $04, MB_ADDR_H = $Cn so
    // `lda (MB_ADDR_L),Y` with Y=0 reads $Cn04. We park the program at
    // $0300 (DOS scratch — no ROM, RAM is writable on a bare Memory).
    mem.memWrite(kMbAddrLZp, 0x04);
    mem.memWrite(kMbAddrHZp,
                 static_cast<uint8_t>(0xC0 + kMockSlot));   // $C400 base
    mem.memWrite(kMbValueZp, 0x00);

    // Detection routine bytes (6 bytes total):
    //   $B1 $10      LDA ($10),Y     5 cycles (no page cross)
    //   $85 $12      STA $12         3 cycles
    //   $B1 $10      LDA ($10),Y     5 cycles
    //   $38          SEC             2 cycles
    //   $E5 $12      SBC $12         3 cycles
    constexpr uint16_t kPC = 0x0300;
    const uint8_t prog[] = {
        0xB1, 0x10,    // LDA ($10),Y
        0x85, 0x12,    // STA $12
        0xB1, 0x10,    // LDA ($10),Y
        0x38,          // SEC
        0xE5, 0x12,    // SBC $12
        0x00,          // BRK (sentinel — test stops here)
    };
    for (size_t i = 0; i < sizeof(prog); ++i) {
        mem.memWrite(static_cast<uint16_t>(kPC + i), prog[i]);
    }
    cpu.setProgramCounter(kPC);
    // Y is 0 from hardReset(); no setter needed.

    // Step through the 5 real instructions (LDA, STA, LDA, SEC, SBC).
    for (int i = 0; i < 5; ++i) cpu.step();

    const uint8_t result = cpu.getAccumulator();
    // Real 6522: between the two T1CL reads exactly 8 CPU cycles pass
    // (5-cycle LDA + 3-cycle STA), so the second read returns a value
    // 8 lower than the first. SBC computes (first - second) = +8, but
    // the routine actually computes (second - first) via the swap of
    // STA/SBC order — re-read: STA stored the FIRST read; SBC subtracts
    // that stored value from A which holds the SECOND read; so
    // A = second - first = -8 = $F8.
    constexpr uint8_t kExpected = 0xF8;
    if (result != kExpected) {
        std::fprintf(stderr,
            "4am detect: expected A=$F8 (= second_T1CL - first_T1CL = -8 "
            "after 8 elapsed cycles), got A=$%02X. Lazy sync did not "
            "advance the VIA's T1 counter between the two MMIO reads — "
            "detection routines will conclude 'no Mockingboard'.\n",
            result);
        return 1;
    }

    std::printf("mockingboard_4am_detect_smoke OK: A=$F8 from "
                "two-LDA-STA-LDA T1CL probe (lazy sync working)\n");
    return 0;
}

}  // namespace

int main() { return main_impl(); }

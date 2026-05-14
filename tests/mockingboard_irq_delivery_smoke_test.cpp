// End-to-end Mockingboard T1 IRQ delivery test — pins the path that
// IRQ-driven music drivers (Ultima IV's voice driver, Nox Archaist's
// Eric Rangell music driver, every PT3 player) depend on.
//
// What we stage:
//   1. Plug a Mockingboard into slot 4 with cpu_ wired.
//   2. Install a tiny IRQ handler at $0400 that bumps a counter byte in
//      zero page and acks T1 (reads T1CL).
//   3. Install $03FE/$03FF (ProDOS-style IRQ vector indirect) and the
//      $FFFE/$FFFF brk/irq vector pointing at a thin trampoline that
//      jumps through $03FE.
//   4. Set up T1: latch = 50 cycles, continuous mode, IER enabled.
//   5. CLI to allow IRQs.
//   6. Run an idle loop (JMP self) at $0500 for a few hundred cycles.
//   7. Assert that the handler ran multiple times — meaning IRQs got
//      delivered from the Mockingboard through the CPU to user code.
//
// Pre-fix behaviour: with batched slice advances, T1 fires only at
// the slice boundary. For a 50-cycle latch and a CPU step of 1
// instruction at a time (~3 cycles per JMP), the CPU runs HUNDREDS
// of instructions before any slice advance happens — meaning the
// IRQ is "frozen" until the next mem.advanceCycles(N) call. The
// lazy-sync added in commit 7848c12 fires on slotRomRead/Write but
// the CPU isn't touching the Mockingboard between IRQs, so the IRQ
// pin only flips at mem.advanceCycles boundaries.

#include "Memory.h"
#include "M6502.h"
#include "Mockingboard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

constexpr int kMockSlot       = 4;
constexpr uint16_t kHandlerPC = 0x0400;
constexpr uint16_t kIdleLoopPC = 0x0500;
constexpr uint16_t kCounterZp = 0x06;   // zero page counter byte

int main_impl()
{
    Memory mem;
    M6502  cpu(&mem);
    mem.setCpu(&cpu);   // installs SlotBus IRQ router

    auto card = std::make_unique<MockingboardCard>(kMockSlot);
    card->setCpu(&cpu); // lazy-sync back-channel for VIA timers
    MockingboardCard* cardPtr = card.get();
    mem.slotBus().plug(kMockSlot, std::move(card));
    cardPtr->onReset();

    // ─── IRQ vector setup ────────────────────────────────────────────
    // $FFFE/$FFFF point at a thin trampoline that JMPs through
    // $03FE/$03FF (matching the ProDOS / DOS 3.3 convention). The
    // trampoline lives at $0600. $03FE/$03FF → user handler at $0400.
    const uint8_t trampoline[] = {
        0x6C, 0xFE, 0x03,   // JMP ($03FE)
    };
    for (size_t i = 0; i < sizeof(trampoline); ++i) {
        mem.memWrite(static_cast<uint16_t>(0x0600 + i), trampoline[i]);
    }
    // $FFFE/$FFFF live in the ROM region (markRomRegion 0xD000..0xFFFF
    // in Memory's constructor), so normal memWrite drops them. Toggle
    // testMode briefly to install the IRQ vector — testMode bypasses
    // ROM protection AND the slot bus, so we have to turn it back off
    // before doing the Mockingboard MMIO setup below.
    mem.setTestMode(true);
    mem.memWrite(0xFFFE, 0x00);
    mem.memWrite(0xFFFF, 0x06);
    mem.setTestMode(false);
    mem.memWrite(0x03FE, static_cast<uint8_t>(kHandlerPC & 0xFF));
    mem.memWrite(0x03FF, static_cast<uint8_t>(kHandlerPC >> 8));

    // ─── IRQ handler ─────────────────────────────────────────────────
    // INC $06 ; LDA $C404 ; RTI    (the LDA acks T1 by reading T1CL)
    const uint8_t handler[] = {
        0xE6, kCounterZp,         // INC $06
        0xAD, 0x04, 0xC4,         // LDA $C404
        0x40,                     // RTI
    };
    for (size_t i = 0; i < sizeof(handler); ++i) {
        mem.memWrite(static_cast<uint16_t>(kHandlerPC + i), handler[i]);
    }

    // ─── Idle loop ───────────────────────────────────────────────────
    // CLI ; loop: JMP loop
    const uint8_t idle[] = {
        0x58,                     // CLI
        0x4C, 0x01, 0x05,         // JMP $0501 (= the JMP itself, tight spin)
    };
    for (size_t i = 0; i < sizeof(idle); ++i) {
        mem.memWrite(static_cast<uint16_t>(kIdleLoopPC + i), idle[i]);
    }

    // ─── Mockingboard T1 setup via SlotBus MMIO ─────────────────────
    // T1 latch = 50, ACR continuous, IER enable T1. Done via direct
    // slot writes so we don't need a CPU prelude.
    auto wMb = [&](uint8_t reg, uint8_t v) {
        // SlotBus device-select window is $C080+slot*16 .. $C08F+slot*16,
        // but VIA registers are accessed via the slot ROM window
        // $Cn00..$CnFF. memWrite routes there.
        mem.memWrite(static_cast<uint16_t>(0xC400 | reg), v);
    };
    wMb(0x06, 50);        // T1LL = 50
    wMb(0x07, 0);         // T1LH = 0  (also transfers latch to counter,
                          //            clears IFR.T1, arms T1)
    wMb(0x0B, 0x40);      // ACR = $40 continuous, no PB7 output
    wMb(0x05, 0);         // T1CH again — load + start (latch = 50)
    wMb(0x0E, 0xC0);      // IER = $C0 → bit7+bit6 → enable T1 IRQ

    // ─── Run the idle loop ──────────────────────────────────────────
    mem.memWrite(kCounterZp, 0x00);
    cpu.hardReset();
    cpu.setProgramCounter(kIdleLoopPC);

    // Each JMP is 3 cycles. With T1 firing every 50 cycles, we should
    // see roughly (steps * 3) / 50 IRQs. Step 1000 times — expect at
    // least 20 IRQs.
    const int kSteps = 1000;
    for (int i = 0; i < kSteps; ++i) cpu.step();

    const uint8_t irqCount = mem.memRead(kCounterZp);
    if (irqCount < 10) {
        std::fprintf(stderr,
            "IRQ delivery: counter = %u after %d steps (≈%d cycles); "
            "expected ≥10 IRQ-handler invocations. T1 IRQs aren't "
            "reaching the CPU — music drivers (Ultima IV, Nox Archaist) "
            "will be silent for exactly this reason.\n",
            irqCount, kSteps, kSteps * 3);
        return 1;
    }

    std::printf("mockingboard_irq_delivery_smoke OK: %u IRQ handler "
                "invocations in %d CPU steps\n", irqCount, kSteps);
    return 0;
}

}  // namespace

int main() { return main_impl(); }

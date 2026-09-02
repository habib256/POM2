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

// Pins the Mockingboard's lazy timer-sync against the Nox Archaist /
// Skyfox / Broadside detection-failure class.
//
// On real hardware the 6522 VIA's T1/T2 counters tick once per Φ2 cycle.
// POM2's host loop advances slot peripherals in batches at the end of
// each CPU run-slice (~17 045 cycles in default mode). Without help,
// a guest sequence that writes T1 and reads IFR a few cycles later
// would always see IFR.T1=0 because no batch advance happened in that
// window — the canonical "is there a Mockingboard?" probe.
//
// The fix in `Mockingboard.cpp::syncToCpuCycle()` runs before every
// `slotRomRead/Write`, computing the delta from `lastSyncCycle_` to
// `cpu_->getCycleCountNow()` and advancing both VIAs through that
// range. This test stages the exact bug:
//
//   1. Plug a CPU back-pointer into the card.
//   2. Write a small T1 latch + enable T1 IRQ in continuous mode.
//   3. Bump Memory::cycleCounter by enough to underflow T1 — WITHOUT
//      ever calling `card.advanceCycles()` (the card isn't plugged
//      into the SlotBus in this harness).
//   4. Read IFR via slotRomRead. Pre-fix this would return 0; post-fix
//      bit 6 is set and `isIrqAsserted()` reports the line asserted.
//
// A negative control verifies that the legacy fallback path (cpu_ ==
// nullptr) still ticks correctly via the batched `advanceCycles()`, so
// the existing `mockingboard_smoke` keeps its semantics.

#include "Mockingboard.h"
#include "Memory.h"
#include "M6502.h"
#include "SlotBus.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>

namespace {

void writeVia(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    card.slotRomWrite(static_cast<uint8_t>(base | (reg & 0x0F)), v);
}

uint8_t readVia(MockingboardCard& card, int chip, uint8_t reg)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    return card.slotRomRead(static_cast<uint8_t>(base | (reg & 0x0F)));
}

void armT1(MockingboardCard& card, uint16_t latch)
{
    writeVia(card, 0, 0x06, static_cast<uint8_t>(latch & 0xFF));      // T1LL
    writeVia(card, 0, 0x07, static_cast<uint8_t>((latch >> 8) & 0xFF)); // T1LH
    writeVia(card, 0, 0x0B, 0x40);                                    // ACR continuous
    // T1CH write transfers latch → counter and arms the timer.
    writeVia(card, 0, 0x05, static_cast<uint8_t>((latch >> 8) & 0xFF));
    writeVia(card, 0, 0x0E, 0xC0);                                    // IER enable T1
}

// ─── Test 1: with CPU back-pointer, lazy sync sees the underflow ──────
bool testLazySyncOnReadIfr()
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);     // for lazy-sync getCycleCountNow back-channel
    card.onReset();   // re-anchors lastSyncCycle_ to current cycle

    // Tight T1 + small latch — the exact pattern detection routines use.
    armT1(card, 50);

    // Spin Memory's clock forward without dispatching to the card.
    // `Memory::advanceCycles` also forwards to plugged slot peripherals,
    // but this card was never plugged into mem.slotBus(), so the only
    // visible effect is that `Memory::cycleCounter` jumps. Pre-fix the
    // VIA would still hold its just-loaded counter value here.
    mem.advanceCycles(200);

    // Read IFR. `slotRomRead` calls `syncToCpuCycle()`, which sees
    // delta ≈ 200 cycles, advances both VIAs through that range, and
    // the T1 underflow trips IFR.T1.
    const uint8_t ifr = readVia(card, 0, 0x0D);
    if ((ifr & 0x40) == 0) {
        std::fprintf(stderr,
            "lazy-sync: IFR.T1 not set after 200-cycle gap "
            "(got 0x%02X) — detection routines will mis-conclude "
            "'no Mockingboard'\n", ifr);
        return false;
    }
    if (!card.isIrqAsserted()) {
        std::fprintf(stderr,
            "lazy-sync: slot IRQ line not asserted after T1 fire\n");
        return false;
    }
    return true;
}

// ─── Test 2: without CPU back-pointer, legacy batched advance still ticks ─
bool testFallbackBatchedAdvance()
{
    MockingboardCard card(4);
    // Deliberately NOT setting cpu — exercises the `cpu_ == nullptr`
    // branch in `advanceCycles()` so we keep the existing
    // mockingboard_smoke test's semantics.
    card.onReset();
    armT1(card, 50);

    // Pre-advance: T1 counter loaded with 50, hasn't underflowed yet.
    assert(!card.isIrqAsserted());

    card.advanceCycles(80);   // walk past the underflow

    if (!card.isIrqAsserted()) {
        std::fprintf(stderr,
            "fallback: legacy batched advance broke T1 IRQ "
            "(no CPU back-pointer path)\n");
        return false;
    }
    if ((card.peekViaRegister(0, 0x0D) & 0x40) == 0) {
        std::fprintf(stderr,
            "fallback: IFR.T1 not visible after batched advance\n");
        return false;
    }
    return true;
}

// ─── Test 3: sync stays correct across mixed access patterns ──────────
bool testSyncAcrossMultipleAccesses()
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.onReset();

    armT1(card, 100);
    // Sequence: advance 30 cycles, read T1CL (clears IFR.T1 if set),
    // advance another 100 cycles (now > 100, underflow happens), read IFR.
    mem.advanceCycles(30);
    (void)readVia(card, 0, 0x04);   // T1CL read at delta=30 — counter ~70
    mem.advanceCycles(100);
    const uint8_t ifr = readVia(card, 0, 0x0D);
    if ((ifr & 0x40) == 0) {
        std::fprintf(stderr,
            "mixed: IFR.T1 not set after second 100-cycle gap "
            "(got 0x%02X)\n", ifr);
        return false;
    }
    return true;
}

// ─── Test 4: end-of-step batch sync must not overshoot by one instruction ─
// Regression for the advanceCycles double-count AND the MMIO access-cycle
// sync. Two things must hold over any window: (1) Memory::advanceCycles folds
// the slice into cycleCounter BEFORE dispatching to the card while
// M6502::step() still holds cpu->cycles == that slice, so a naive batch
// getCycleCountNow() overshoots "now" by one instruction — corrected in
// MockingboardCard::advanceCycles; (2) a lazy MMIO sync lands on the access's
// DATA cycle (syncToCpuCycle() = getCycleCountNow()+1), because cpu->cycles
// does not yet count the in-flight data cycle. Both are true only when the
// access rides a REAL instruction, so drive everything through cpu.step():
// over N elapsed CPU cycles — NOPs plus an executed `LDA $C404` MMIO read —
// the VIA must advance exactly N. A regression of either bug advances it a
// different amount than the CPU clock. (An earlier version armed via direct
// slotRomWrite calls at cpu->cycles == 0; that placed the +1 access-cycle
// sync one cycle off a CPU-less reference — an artifact of the synthetic
// arm, not of real operation.)
bool testNoEndOfStepOvershoot()
{
    auto t1Counter = [](MockingboardCard& c) -> uint16_t {
        return static_cast<uint16_t>(c.peekViaRegister(0, 0x04)) |
               static_cast<uint16_t>(c.peekViaRegister(0, 0x05) << 8);
    };

    Memory mem;
    M6502  cpu(&mem);
    auto cardp = std::make_unique<MockingboardCard>(4);
    cardp->setCpu(&cpu);
    MockingboardCard* card = cardp.get();
    mem.slotBus().plug(4, std::move(cardp));
    cpu.hardReset();
    mem.slotBus().reset();

    // Program at $0300: arm T1 continuous (latch 1000 = $03E8, no underflow
    // in-window) via executed stores, then NOPs, an executed T1CL read, NOPs.
    uint16_t p = 0x0300;
    auto emit = [&](std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) mem.memWrite(p++, b);
    };
    emit({0xA9, 0xE8, 0x8D, 0x06, 0xC4});   // LDA #$E8 ; STA $C406 (T1LL)
    emit({0xA9, 0x03, 0x8D, 0x07, 0xC4});   // LDA #$03 ; STA $C407 (T1LH)
    emit({0xA9, 0x40, 0x8D, 0x0B, 0xC4});   // LDA #$40 ; STA $C40B (ACR continuous)
    emit({0xA9, 0x03, 0x8D, 0x05, 0xC4});   // LDA #$03 ; STA $C405 (T1CH arms)
    const uint16_t armEnd = p;
    for (int i = 0; i < 8; ++i) emit({0xEA});
    emit({0xAD, 0x04, 0xC4});               // LDA $C404 (executed MMIO T1CL read)
    for (int i = 0; i < 8; ++i) emit({0xEA});
    const uint16_t progEnd = p;

    cpu.setProgramCounter(0x0300);
    while (cpu.getProgramCounter() < armEnd) cpu.step();   // arm complete
    const uint16_t c0 = t1Counter(*card);
    const uint64_t t0 = mem.getCycleCounter();
    while (cpu.getProgramCounter() < progEnd) cpu.step();  // NOPs + the MMIO read
    const uint16_t c1 = t1Counter(*card);
    const uint64_t elapsed = mem.getCycleCounter() - t0;

    const uint16_t advanced = static_cast<uint16_t>(c0 - c1);
    if (advanced != elapsed) {
        std::fprintf(stderr, "end-of-step overshoot: VIA advanced %u over %llu "
                     "elapsed CPU cycles (MMIO access-cycle sync / batch "
                     "diverged)\n", static_cast<unsigned>(advanced),
                     (unsigned long long)elapsed);
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= testLazySyncOnReadIfr();
    ok &= testFallbackBatchedAdvance();
    ok &= testSyncAcrossMultipleAccesses();
    ok &= testNoEndOfStepOvershoot();
    if (!ok) return 1;
    std::printf("mockingboard_sync_smoke OK: lazy timer-sync + legacy "
                "fallback both wired\n");
    return 0;
}

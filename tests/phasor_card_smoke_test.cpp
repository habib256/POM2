// PhasorCard smoke test — pins the four observable surfaces of the
// dual-mode Phasor card:
//
//   1. VIA register layout — both VIAs respond at their decoded ranges
//      ($Cs00-$Cs0F + mirrors, $Cs80-$Cs8F + mirrors). Writing T1CH
//      then reading T1CL roundtrips with cycle-accounting decremented.
//   2. Mode soft-switch ($C0(8+s)X) — boots in PH_Mockingboard, a
//      device-select read/write at $C0(8+s)D transitions to PH_Phasor
//      (mode=5), $C0(8+s)8 clears back to PH_Mockingboard (mode=0).
//      Both reads AND writes trigger the switch (AppleWin behaviour).
//   3. MB-compat routing — in PH_Mockingboard the primary AY of each
//      VIA pair (AY0 for VIA1, AY2 for VIA2) receives the LATCH+WRITE
//      strobes; the secondary AYs (AY1, AY3) stay untouched even when
//      the chip-select bits (PB3/PB4) say otherwise.
//   4. Phasor-native routing — in PH_Phasor the active-low chip-select
//      decode `chip_sel = (~(pb >> 3)) & 3` honours PB3 (primary) and
//      PB4 (secondary): primary-only writes hit AY0 (not AY1);
//      secondary-only hit AY1 (not AY0); broadcast hits both. VIA2's
//      strobes land on the AY2/AY3 pair.

#include "AudioDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "PhasorCard.h"
#include "SlotBus.h"
#include "Via6522.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

// Build a Port B byte: BC1=bc1, BDIR=bdir, /RESET=1 (chip running),
// PB3=cs0 (primary select, active LOW so pass 0 to select), PB4=cs1
// (secondary select), other bits = 1.
uint8_t makePb(bool bc1, bool bdir, bool selPrimary, bool selSecondary)
{
    uint8_t pb = 0xFF;                            // start all high
    if (bc1)        pb |= 0x01; else pb &= ~0x01;
    if (bdir)       pb |= 0x02; else pb &= ~0x02;
    pb |= 0x04;                                   // /RESET=1 (no reset)
    if (selPrimary)   pb &= ~0x08; else pb |= 0x08; // PB3 active-low
    if (selSecondary) pb &= ~0x10; else pb |= 0x10; // PB4 active-low
    return pb;
}

// Drive one LATCH+WRITE strobe through `viaIdx`: latch AY register
// `regAddr`, then write `data`. Both AYs in the pair will receive
// these depending on the mode + chip-select bits in `pb`.
void doLatchWrite(PhasorCard& card, int viaIdx, uint8_t pb,
                  uint8_t regAddr, uint8_t data)
{
    const uint8_t base = (viaIdx == 0) ? 0x00 : 0x80;
    // DDRA = all output, DDRB = all output.
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRA, 0xFF);
    card.slotRomWrite(base + pom2::Via6522::VIA_DDRB, 0xFF);
    // LATCH ADDR phase: BDIR=1, BC1=1, plus chip-select bits.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  regAddr);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x03));
    // INACTIVE (BDIR=0, BC1=0) — drop the strobe so the next one is an edge.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));
    // WRITE phase: BDIR=1, BC1=0, plus chip-select bits.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORA,  data);
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>((pb & ~0x03) | 0x02));
    // INACTIVE again so a subsequent LATCH is an edge.
    card.slotRomWrite(base + pom2::Via6522::VIA_ORB,
                      static_cast<uint8_t>(pb & ~0x03));
}

void testViaLayout()
{
    PhasorCard card(4);
    // VIA1 lives at slot ROM low8 = 0x00..0x7F (mirrors of 0x00..0x0F).
    // VIA2 lives at low8 = 0x80..0xFF.
    // Distinct T1 latches written to VIA1 vs VIA2 must come back separate.
    card.slotRomWrite(pom2::Via6522::VIA_T1LL,        0x34);
    card.slotRomWrite(pom2::Via6522::VIA_T1LH,        0x12);
    card.slotRomWrite(0x80 + pom2::Via6522::VIA_T1LL, 0x78);
    card.slotRomWrite(0x80 + pom2::Via6522::VIA_T1LH, 0x56);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0x34);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LH) == 0x12);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LL) == 0x78);
    assert(card.peekViaRegister(1, pom2::Via6522::VIA_T1LH) == 0x56);
    // Mirror: low8 = 0x40 should also reach VIA1 (high bit clear).
    card.slotRomWrite(0x40 + pom2::Via6522::VIA_T1LL, 0xAB);
    assert(card.peekViaRegister(0, pom2::Via6522::VIA_T1LL) == 0xAB);

    std::printf("  ok: dual-VIA register layout + mirror decode\n");
}

void testModeSoftSwitch()
{
    PhasorCard card(4);
    // Power-up = PH_Mockingboard.
    assert(card.mode() == PhasorCard::PH_Mockingboard);
    assert(card.clockScale() == 1);

    // Write to $C0(8+s)D: low4 = 0xD = 0b1101 → bit3 set (clear mode),
    // low3 = 0b101 = 5 → PH_Phasor.
    card.deviceSelectWrite(0xD, 0x00);
    assert(card.mode() == PhasorCard::PH_Phasor);
    assert(card.clockScale() == 2);

    // Write to $C0(8+s)8: low4 = 0x8 = 0b1000 → clear mode, low3 = 0
    // → PH_Mockingboard.
    card.deviceSelectWrite(0x8, 0x00);
    assert(card.mode() == PhasorCard::PH_Mockingboard);

    // Read also triggers the switch (AppleWin behaviour). Read $C0(8+s)F
    // → low4 = 0xF → clear + OR 7 → PH_EchoPlus.
    const uint8_t status = card.deviceSelectRead(0xF);
    assert(card.mode() == PhasorCard::PH_EchoPlus);
    assert(status == PhasorCard::PH_EchoPlus);

    // Read $C0(8+s)0 (no bit3, no OR) keeps current mode unchanged.
    (void)card.deviceSelectRead(0x0);
    assert(card.mode() == PhasorCard::PH_EchoPlus);

    // Read $C0(8+s)5 (no bit3, OR 5) ORs bits in: 7 | 5 = 7, still EP.
    (void)card.deviceSelectRead(0x5);
    assert(card.mode() == PhasorCard::PH_EchoPlus);

    std::printf("  ok: mode soft-switch ($C0(8+s)X bit3 clears, low3 ORs)\n");
}

void testMockingboardCompatRouting()
{
    PhasorCard card(4);
    // PH_Mockingboard at reset.
    assert(card.mode() == PhasorCard::PH_Mockingboard);

    // VIA1 LATCH+WRITE to AY register 7 (mixer control), data $1F.
    // Pass chip-select bits that "would" select BOTH in Phasor mode
    // (PB3=0, PB4=0) — MB compat must ignore them and only hit AY0.
    const uint8_t pbBoth = makePb(/*bc1*/0, /*bdir*/0, /*pri*/1, /*sec*/1);
    doLatchWrite(card, /*via*/0, pbBoth, /*reg*/7, /*data*/0x1F);

    // AY0 received the write; AY1 stayed silent.
    assert(card.getAyRegister(0, 7) == 0x1F);
    assert(card.getAyRegister(1, 7) == 0x00);

    // Same for VIA2: AY2 receives, AY3 untouched.
    doLatchWrite(card, /*via*/1, pbBoth, /*reg*/8, /*data*/0x0F);
    assert(card.getAyRegister(2, 8) == 0x0F);
    assert(card.getAyRegister(3, 8) == 0x00);

    std::printf("  ok: MB-compat mode routes to primary AY only (PB3/PB4 ignored)\n");
}

void testPhasorNativeRouting()
{
    PhasorCard card(4);
    // Switch to PH_Phasor.
    card.deviceSelectWrite(0xD, 0);
    assert(card.mode() == PhasorCard::PH_Phasor);

    // VIA1 → AY0 only (PB3 low, PB4 high).
    const uint8_t pbPri = makePb(0, 0, /*pri*/1, /*sec*/0);
    doLatchWrite(card, 0, pbPri, /*reg*/0, /*data*/0xAA);
    assert(card.getAyRegister(0, 0) == 0xAA);
    assert(card.getAyRegister(1, 0) == 0x00);

    // VIA1 → AY1 only (PB3 high, PB4 low).
    const uint8_t pbSec = makePb(0, 0, /*pri*/0, /*sec*/1);
    doLatchWrite(card, 0, pbSec, /*reg*/1, /*data*/0xBB);
    assert(card.getAyRegister(0, 1) == 0x00);
    assert(card.getAyRegister(1, 1) == 0xBB);

    // VIA1 → BOTH (PB3 low, PB4 low).
    const uint8_t pbBoth = makePb(0, 0, /*pri*/1, /*sec*/1);
    doLatchWrite(card, 0, pbBoth, /*reg*/2, /*data*/0xCC);
    assert(card.getAyRegister(0, 2) == 0xCC);
    assert(card.getAyRegister(1, 2) == 0xCC);

    // VIA1 → NONE (PB3 high, PB4 high).
    const uint8_t pbNone = makePb(0, 0, /*pri*/0, /*sec*/0);
    doLatchWrite(card, 0, pbNone, /*reg*/3, /*data*/0xDD);
    assert(card.getAyRegister(0, 3) == 0x00);
    assert(card.getAyRegister(1, 3) == 0x00);

    // VIA2 → AY3 only (secondary).
    doLatchWrite(card, 1, pbSec, /*reg*/4, /*data*/0xEE);
    assert(card.getAyRegister(2, 4) == 0x00);
    assert(card.getAyRegister(3, 4) == 0xEE);

    std::printf("  ok: Phasor-native chip-select PB3/PB4 active-low decode\n");
}

void testAudioSynth4Chips()
{
    PhasorCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);
    card.deviceSelectWrite(0xD, 0);    // → PH_Phasor (4 active chips)

    // Drive each of the 4 chips with a tone via the appropriate VIA +
    // chip-select. AY0 + AY1 reached via VIA1 with primary / secondary
    // selects; AY2 + AY3 via VIA2 similarly.
    auto configChannelA = [&](int viaIdx, bool selPrimary,
                              uint8_t periodLo, uint8_t periodHi)
    {
        const uint8_t pb = makePb(0, 0, selPrimary, !selPrimary);
        // R0/R1 = channel A period (12-bit).
        doLatchWrite(card, viaIdx, pb, /*reg*/0, periodLo);
        doLatchWrite(card, viaIdx, pb, /*reg*/1, periodHi);
        // R7 = mixer: enable tone A only (bit 0 = 0), tone B/C off,
        // all noise off. R7 = 0b00111110 = 0x3E.
        doLatchWrite(card, viaIdx, pb, /*reg*/7, 0x3E);
        // R8 = chan A amplitude = 15.
        doLatchWrite(card, viaIdx, pb, /*reg*/8, 0x0F);
    };
    // Different periods per chip so each plays a distinct pitch.
    configChannelA(0, /*pri*/true,  0x00, 0x01);   // AY0: period $100
    configChannelA(0, /*pri*/false, 0x80, 0x01);   // AY1: period $180
    configChannelA(1, /*pri*/true,  0x00, 0x02);   // AY2: period $200
    configChannelA(1, /*pri*/false, 0x80, 0x02);   // AY3: period $280

    constexpr int N = 8192;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), N);

    // RMS energy must be non-trivial — all 4 chips contribute amplitude.
    double sumSq = 0.0;
    float vmin = +1e9f, vmax = -1e9f;
    for (float s : buf) {
        sumSq += static_cast<double>(s) * s;
        if (s < vmin) vmin = s;
        if (s > vmax) vmax = s;
    }
    const double rms = std::sqrt(sumSq / N);
    std::printf("  4-AY Phasor mix rms=%.4f vmin=%.4f vmax=%.4f\n",
                rms, vmin, vmax);
    // 4 chips × amp 15 × peak 1.0 / 12 (mix div) → ≥ 0.10 RMS easily.
    // Set the floor low enough to survive minor synth tweaks.
    assert(rms > 0.05);

    // Mute path silences everything.
    card.setMuted(true);
    src->fillAudioBuffer(buf.data(), N);
    sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    assert(sumSq == 0.0);

    std::printf("  ok: 4-AY mix produces non-silent waveform; mute path silences\n");
}

// Estimate dominant tone frequency from a buffer via zero-crossings.
// Mockingboard / Phasor square-wave outputs are DC-offset (not centred);
// count crossings of the running mean for a robust estimate.
double estimateFreqHz(const std::vector<float>& buf, uint32_t sr)
{
    double mean = 0;
    for (float s : buf) mean += s;
    mean /= buf.size();
    int crossings = 0;
    bool above = (buf[0] - mean) >= 0.0f;
    for (size_t i = 1; i < buf.size(); ++i) {
        const bool now = (buf[i] - mean) >= 0.0f;
        if (now != above) ++crossings;
        above = now;
    }
    // 2 crossings per full cycle.
    return (crossings * 0.5) * sr / buf.size();
}

void testClockScaleDoublesPitch()
{
    // Same register values in MB vs Phasor mode must produce 2x pitch
    // in Phasor mode (clockScale=2 doubles the AY input clock).
    constexpr int N = 16384;
    constexpr uint32_t SR = 44100;

    auto playOneAyToneAndMeasure = [](PhasorCard::Mode mode) -> double {
        PhasorCard card(4);
        card.setSampleRate(SR);
        card.setVolume(1.0f);
        card.setMuted(false);
        // Switch into target mode. PH_Mockingboard is the power-up
        // state, so we only need to write for PH_Phasor.
        if (mode == PhasorCard::PH_Phasor) card.deviceSelectWrite(0xD, 0);
        assert(card.mode() == mode);

        // Drive AY0 via VIA1 primary. In MB-compat mode chip-select bits
        // are ignored so primary always wins → AY0; in Phasor mode we
        // explicitly select primary for the same target chip.
        const uint8_t pb = makePb(0, 0, /*pri*/true, /*sec*/false);
        // Period = $200 → tone freq at clock/16/$200 = ~125 Hz @ MB rate,
        // ~250 Hz @ Phasor-native rate.
        doLatchWrite(card, 0, pb, 0, 0x00);
        doLatchWrite(card, 0, pb, 1, 0x02);
        doLatchWrite(card, 0, pb, 7, 0x3E);   // enable tone A only
        doLatchWrite(card, 0, pb, 8, 0x0F);   // amp 15

        std::vector<float> buf(N);
        card.audioSource()->fillAudioBuffer(buf.data(), N);
        return estimateFreqHz(buf, SR);
    };

    const double mbFreq = playOneAyToneAndMeasure(PhasorCard::PH_Mockingboard);
    const double phFreq = playOneAyToneAndMeasure(PhasorCard::PH_Phasor);

    std::printf("  tone @ period $200: MB=%.1f Hz  Phasor=%.1f Hz  ratio=%.3f\n",
                mbFreq, phFreq, (mbFreq > 0 ? phFreq / mbFreq : 0));
    // Both must be in the audible range and the ratio must be roughly 2.
    // Allow a generous tolerance (±15%) — zero-crossing estimation is
    // noisy on a single-channel square wave and the AY counter's
    // sub-tick aliasing wobbles the apparent freq.
    assert(mbFreq > 50.0 && mbFreq < 200.0);
    assert(phFreq > 100.0 && phFreq < 400.0);
    assert(phFreq / mbFreq > 1.7 && phFreq / mbFreq < 2.3);

    std::printf("  ok: clockScale() in Phasor mode doubles AY tone frequency\n");
}

void testTelemetry()
{
    PhasorCard card(4);
    card.deviceSelectWrite(0xD, 0);    // → PH_Phasor

    const uint8_t pbPri = makePb(0, 0, /*pri*/1, /*sec*/0);
    doLatchWrite(card, 0, pbPri, /*reg*/5, /*data*/0x11);
    doLatchWrite(card, 0, pbPri, /*reg*/6, /*data*/0x22);

    // Two writes landed on AY0 (regs 5, 6), zero on AY1/AY2/AY3.
    assert(card.getAyWriteCount(0) == 2);
    assert(card.getAyWriteCount(1) == 0);
    assert(card.getAyWriteCount(2) == 0);
    assert(card.getAyWriteCount(3) == 0);
    // VIA0 saw a bunch of MMIO writes (DDR setup + 6 strobes per
    // doLatchWrite × 2 = 12+), VIA1 saw none.
    assert(card.getViaWriteCount(0) > 0);
    assert(card.getViaWriteCount(1) == 0);

    std::printf("  ok: per-AY + per-VIA telemetry counters\n");
}

// Regression for the end-of-step overshoot. Memory::advanceCycles folds
// the slice into cycleCounter BEFORE dispatching to the card, yet
// M6502::step() still holds cpu->cycles == that slice, so
// getCycleCountNow() (= cycleCounter + cpu->cycles) overshoots "now" by
// one instruction. Pre-fix PhasorCard::advanceCycles compensated with
// syncToCpuCycleAt(now - cycles) but THEN also called via_->advance(cycles)
// a second time, double-charging T1 by one slice per call. Drive the
// same elapsed cycles two ways and require the T1 counter to match.
void testNoEndOfStepOvershoot()
{
    constexpr uint16_t kLatch  = 1000;
    constexpr uint64_t kCycles = 40;

    auto t1Counter = [](PhasorCard& c) -> uint16_t {
        return static_cast<uint16_t>(c.peekViaRegister(0, 0x04)) |
               static_cast<uint16_t>(c.peekViaRegister(0, 0x05) << 8);
    };
    auto armT1 = [](PhasorCard& c, uint16_t latch) {
        c.slotRomWrite(pom2::Via6522::VIA_T1LL,
                       static_cast<uint8_t>(latch & 0xFF));
        c.slotRomWrite(pom2::Via6522::VIA_T1LH,
                       static_cast<uint8_t>((latch >> 8) & 0xFF));
        c.slotRomWrite(pom2::Via6522::VIA_ACR, 0x40);    // continuous
        c.slotRomWrite(pom2::Via6522::VIA_T1CH,
                       static_cast<uint8_t>((latch >> 8) & 0xFF));
        c.slotRomWrite(pom2::Via6522::VIA_IER, 0xC0);    // enable T1
    };

    // Reference: legacy batched advance (no CPU back-pointer).
    PhasorCard ref(4);
    ref.onReset();
    armT1(ref, kLatch);
    ref.advanceCycles(static_cast<int>(kCycles));
    const uint16_t refCtr = t1Counter(ref);

    // CPU-driven: same elapsed cycles via real M6502 NOP stepping through
    // the Memory::advanceCycles -> slotBus -> card path.
    Memory mem;
    M6502  cpu(&mem);
    auto cardp = std::make_unique<PhasorCard>(4);
    cardp->setCpu(&cpu);
    PhasorCard* card = cardp.get();
    mem.slotBus().plug(4, std::move(cardp));
    cpu.hardReset();
    mem.slotBus().reset();
    for (uint16_t a = 0x0300; a < 0x0360; ++a) mem.memWrite(a, 0xEA); // NOPs
    armT1(*card, kLatch);
    cpu.setProgramCounter(0x0300);
    const uint64_t start = mem.getCycleCounter();
    while (mem.getCycleCounter() - start < kCycles) cpu.step();
    const uint64_t elapsed = mem.getCycleCounter() - start;
    const uint16_t cpuCtr = t1Counter(*card);

    if (elapsed != kCycles) {
        std::fprintf(stderr, "Phasor overshoot test: NOP stepping landed at "
                     "%llu cycles (want %llu)\n",
                     (unsigned long long)elapsed,
                     (unsigned long long)kCycles);
        std::abort();
    }
    if (cpuCtr != refCtr) {
        std::fprintf(stderr, "Phasor end-of-step overshoot: CPU-driven T1=%u "
                     "!= batch-driven=%u after %llu cycles\n",
                     cpuCtr, refCtr, (unsigned long long)elapsed);
        std::abort();
    }
    std::printf("  ok: CPU-driven advance matches batch (T1=%u, %llu cycles)\n",
                cpuCtr, (unsigned long long)elapsed);
}

} // namespace

int main()
{
    std::printf("PhasorCard smoke test\n");
    testViaLayout();
    testModeSoftSwitch();
    testMockingboardCompatRouting();
    testPhasorNativeRouting();
    testAudioSynth4Chips();
    testClockScaleDoublesPitch();
    testTelemetry();
    testNoEndOfStepOvershoot();
    std::printf("PASS\n");
    return 0;
}

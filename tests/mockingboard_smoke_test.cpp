// Mockingboard smoke test — pins the four behaviours that any music
// driver running on a real Apple II Mockingboard A/C depends on:
//
//   1. Slot ROM address decode: writes to $Cn0X reach VIA #1, writes to
//      $Cn8X reach VIA #2.
//   2. VIA → AY register addressing. The canonical command sequence
//      LATCH ($Cn00 ← addr ; $Cn02 ← 0x07) → WRITE ($Cn00 ← data ;
//      $Cn02 ← 0x06) → INACTIVE deposits `data` in the addressed AY
//      register. We assert this end-to-end through `slotRomWrite`.
//   3. VIA Timer 1 IRQ generation. Continuous mode plus IER = $C0
//      (T1 enabled) must drive `isIrqAsserted()` true after the first
//      timer underflow, and the IRQ stays asserted until $Cn04 (T1CL)
//      read clears IFR.T1.
//   4. AY synthesis produces a non-silent waveform when a tone period
//      is set and the channel + amplitude are enabled.
//
// Self-contained: doesn't touch Memory or M6502, drives the card's
// SlotPeripheral interface directly. Mirrors the structure of
// `clock_card_smoke_test.cpp`.

#include "Mockingboard.h"

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// Convenience wrappers — the card's slot-ROM API is byte-indexed within
// a 256-byte window, but writers think in $CnXX addresses. The card
// doesn't care which slot it's in (it only sees the low byte), so we
// just feed the offset directly.

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

// AY command codes carried on VIA Port B bits 0..2:
//   bit 0 = !RESET (active low)        — 0x01 means "not reset"
//   bit 1 = BDIR
//   bit 2 = BC1
constexpr uint8_t kPbInactive = 0x01;        // {BC1=0, BDIR=0, !RESET=1}
constexpr uint8_t kPbWrite    = 0x03;        // {BC1=0, BDIR=1, !RESET=1}
constexpr uint8_t kPbLatch    = 0x07;        // {BC1=1, BDIR=1, !RESET=1}

// Drive the canonical 6522→AY bus sequence to set AY register `reg` to
// `value` on chip 0 or 1. Mirrors the AppleWin / WozTracker idiom.
void ayWrite(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    // Configure DDRs once: PA all output, PB lower 3 bits output. (A
    // real driver does this once at init; we do it on every call here
    // so the test functions are self-contained.)
    writeVia(card, chip, 0x03, 0xFF);   // DDRA = $FF
    writeVia(card, chip, 0x02, 0x07);   // DDRB = $07 (PB0..PB2 output)

    // Latch register address.
    writeVia(card, chip, 0x01, reg);          // ORA = reg
    writeVia(card, chip, 0x00, kPbLatch);     // ORB = LATCH
    writeVia(card, chip, 0x00, kPbInactive);  // ORB = INACTIVE
    // Write data.
    writeVia(card, chip, 0x01, v);            // ORA = v
    writeVia(card, chip, 0x00, kPbWrite);     // ORB = WRITE
    writeVia(card, chip, 0x00, kPbInactive);  // ORB = INACTIVE
}

// ─── Test 1: address decoding ────────────────────────────────────────────
void testAddressDecode()
{
    MockingboardCard card(4);
    // Set DDRA = $FF on both VIAs so reads of ORA reflect the latch
    // (with DDR=0, input lines float high and the read is always $FF).
    card.slotRomWrite(0x03, 0xFF);     // VIA1 DDRA = $FF
    card.slotRomWrite(0x83, 0xFF);     // VIA2 DDRA = $FF
    // Write a sentinel to VIA1 ORA; VIA2 ORA must be unaffected.
    card.slotRomWrite(0x01, 0xAA);     // VIA1 ORA
    card.slotRomWrite(0x81, 0x55);     // VIA2 ORA
    assert(card.peekViaRegister(0, 0x01) == 0xAA);
    assert(card.peekViaRegister(1, 0x01) == 0x55);
    // Mirror behaviour: $Cn10 should also decode to VIA1 (low 4 bits =
    // reg, high bit alone selects chip).
    card.slotRomWrite(0x11, 0x33);
    assert(card.peekViaRegister(0, 0x01) == 0x33);
    // $Cn90 mirrors VIA2.
    card.slotRomWrite(0x91, 0x44);
    assert(card.peekViaRegister(1, 0x01) == 0x44);
}

// ─── Test 2: AY register write via VIA control bus ────────────────────────
void testAyRegisterWrite()
{
    MockingboardCard card(4);
    // Drive the standard sequence: AY chip 0, R0 (ch A tone period low) ← $5A.
    ayWrite(card, 0, /*reg=*/0, /*v=*/0x5A);
    assert(card.getAyRegister(0, 0) == 0x5A);

    // R7 (mixer) ← $38 (tone enabled on all 3 chans, noise off).
    ayWrite(card, 0, /*reg=*/7, /*v=*/0x38);
    assert(card.getAyRegister(0, 7) == 0x38);

    // Different chip / different register.
    ayWrite(card, 1, /*reg=*/8, /*v=*/0x0F);     // chan A amplitude max
    assert(card.getAyRegister(1, 8) == 0x0F);

    // Verify the !RESET line zeros the AY: drop PB0, then check.
    writeVia(card, 0, 0x00, 0x00);    // ORB = 0 → !RESET asserted
    assert(card.getAyRegister(0, 0) == 0);
    assert(card.getAyRegister(0, 7) == 0);
}

// ─── Test 3: VIA T1 IRQ generation in continuous mode ────────────────────
void testT1IrqContinuous()
{
    MockingboardCard card(4);
    assert(!card.isIrqAsserted());

    // Set T1 latch to 100 cycles, ACR = $40 (T1 continuous, no PB7),
    // IER = $C0 (set bit 7 + bit 6 = enable T1).
    writeVia(card, 0, 0x06, 100);     // T1LL = 100
    writeVia(card, 0, 0x07, 0);       // T1LH = 0
    writeVia(card, 0, 0x0B, 0x40);    // ACR = continuous
    writeVia(card, 0, 0x05, 0);       // T1CH = 0 (also xfers latch → counter)
    writeVia(card, 0, 0x04, 100);     // T1CL = 100
    writeVia(card, 0, 0x07, 0);       // T1CH must be re-written to actually
    writeVia(card, 0, 0x05, 0);       // start (T1CH write transfers latch).
    writeVia(card, 0, 0x0E, 0xC0);    // IER = enable T1

    // Counter is now ~100. Walk past the underflow.
    card.advanceCycles(150);
    assert(card.isIrqAsserted());
    assert((card.peekViaRegister(0, 0x0D) & 0x40) != 0);  // IFR.T1 set

    // Reading T1CL must clear IFR.T1 and drop the IRQ line.
    (void)readVia(card, 0, 0x04);
    assert((card.peekViaRegister(0, 0x0D) & 0x40) == 0);
    assert(!card.isIrqAsserted());

    // In continuous mode the timer should fire again after another period.
    card.advanceCycles(200);
    assert(card.isIrqAsserted());
}

// ─── Test 4: VIA T1 one-shot fires once and stays low ────────────────────
void testT1IrqOneShot()
{
    MockingboardCard card(4);
    writeVia(card, 0, 0x06, 50);      // T1LL = 50
    writeVia(card, 0, 0x07, 0);       // T1LH = 0
    writeVia(card, 0, 0x0B, 0x00);    // ACR = one-shot
    writeVia(card, 0, 0x05, 0);       // T1CH = 0 → load + start
    writeVia(card, 0, 0x0E, 0xC0);    // IER enable T1

    card.advanceCycles(80);
    assert(card.isIrqAsserted());
    // Clear the flag.
    (void)readVia(card, 0, 0x04);
    assert(!card.isIrqAsserted());
    // One-shot: even after another full period, no fire.
    card.advanceCycles(500);
    assert(!card.isIrqAsserted());
}

// ─── Test 5: AY tone synthesis is non-silent when configured ─────────────
void testAyAudioSynthesis()
{
    MockingboardCard card(4);
    card.setSampleRate(44100);
    card.setVolume(1.0f);
    card.setMuted(false);

    // Configure chip 0, channel A: period 0x100 (~250 Hz), max amplitude,
    // mixer enables tone A only (R7 = 0x3E → tone B/C off, all noise off,
    // tone A on). R7 active-low: bit 0 = 0 enables tone A.
    ayWrite(card, 0, 0,  0x00);    // R0 (period low)
    ayWrite(card, 0, 1,  0x01);    // R1 (period high, 12-bit period = 0x100)
    ayWrite(card, 0, 7,  0x3E);    // R7 mixer
    ayWrite(card, 0, 8,  0x0F);    // R8 channel A amplitude = 15

    // Pull a buffer of audio. Expect a non-silent square-ish waveform.
    constexpr int N = 4096;
    std::vector<float> buf(N);
    AudioSource* src = card.audioSource();
    assert(src);
    src->fillAudioBuffer(buf.data(), N);

    // RMS energy must be appreciably above zero. Silence would give 0.
    double sumSq = 0.0;
    float  vmin = +1e9f, vmax = -1e9f;
    for (float s : buf) {
        sumSq += static_cast<double>(s) * s;
        if (s < vmin) vmin = s;
        if (s > vmax) vmax = s;
    }
    const double rms = std::sqrt(sumSq / N);
    std::printf("  AY ch-A tone @ period $0100: rms=%.4f vmin=%.4f vmax=%.4f\n",
                rms, vmin, vmax);
    // Expect non-trivial energy. The exact value depends on the volume
    // table + the per-sample increment; at amp 15 with one channel
    // active the divide-by-6 in fillAudioBuffer normalises this to
    // ~0.16 mean amplitude.
    assert(rms > 0.05);
    // Mute path silences output completely.
    card.setMuted(true);
    src->fillAudioBuffer(buf.data(), N);
    sumSq = 0.0;
    for (float s : buf) sumSq += static_cast<double>(s) * s;
    assert(sumSq == 0.0);
}

// ─── Test 6: Reset clears VIA + AY state ─────────────────────────────────
void testReset()
{
    MockingboardCard card(4);
    ayWrite(card, 0, 8, 0x0F);
    assert(card.getAyRegister(0, 8) == 0x0F);

    writeVia(card, 0, 0x06, 100);
    writeVia(card, 0, 0x07, 0);
    writeVia(card, 0, 0x0B, 0x40);
    writeVia(card, 0, 0x05, 0);
    writeVia(card, 0, 0x0E, 0xC0);
    card.advanceCycles(200);
    assert(card.isIrqAsserted());

    card.onReset();
    assert(!card.isIrqAsserted());
    assert(card.getAyRegister(0, 8) == 0);
    assert(card.peekViaRegister(0, 0x0E) == 0x80);  // IER reads as ier|0x80
}

}  // namespace

int main()
{
    testAddressDecode();        std::printf("address decode ........ OK\n");
    testAyRegisterWrite();      std::printf("AY register write ..... OK\n");
    testT1IrqContinuous();      std::printf("T1 IRQ continuous ..... OK\n");
    testT1IrqOneShot();         std::printf("T1 IRQ one-shot ....... OK\n");
    testAyAudioSynthesis();     std::printf("AY audio synthesis .... OK\n");
    testReset();                std::printf("reset ................. OK\n");
    std::printf("Mockingboard smoke test passed.\n");
    return 0;
}

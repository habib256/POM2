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

// Mockingboard emuCycles TIMELINE regression test.
//
// `mockingboard_audio_quality` pins what the replay queue does while the
// CPU cycle counter marches forward. This file pins what happens when it
// does NOT — the two ways the cycle-stamped stream stops describing the
// machine, both of which used to leave the card audibly wrong for
// seconds or forever:
//
//   1. THE TIMELINE MOVES BACKWARDS (rewind, snapshot load). The audio
//      thread's `pending` deque is a jitter buffer holding ~40 ms of
//      un-rendered writes, and the render loop consumes it strictly
//      front-ordered. Roll the cycle counter back and every one of those
//      stamps lands in the future of the re-anchored cursor, so the queue
//      front blocks everything behind it and the AY register bank freezes
//      until the CPU has replayed its way back to the old stamps.
//      Measured silence before the fix: 0.49 s / 2.00 s / >3 s for rewind
//      depths of 0.5 / 2 / 5 s, bounded only by the 16384-event queue cap
//      (~16 s on a typical write rate).
//
//   2. THE BANK IS WIPED OUT-OF-BAND (AY /RESET strobe, card reset). The
//      wipe carried no cycle stamp, so the audio thread learned about it
//      through a counter — at CPU-NOW, ~40 ms ahead of its own cursor.
//      The ~40 ms of pre-reset writes still queued behind the cursor then
//      replayed on top of the zeroed bank and put the note back. Nothing
//      followed to change it, so a board the software had just silenced
//      held its last note until the next program reinitialised it.
//
//      This is the "note hangs when DIX switches demos" report, and DIX's
//      own inter-demo loader is where it comes from. `loader.a RESET_AY`
//      (Fr3nchT0uch/DIX, GPLv3) is called from every `LOAD_*` hand-off and
//      silences the board with nothing but the strobe — no volume writes:
//
//          LDY #$03 / LDA #$FF / STA (MB_BASEADDR),Y   ; DDRA1 = $FF
//          DEY      / LDA #$07 / STA (MB_BASEADDR),Y   ; DDRB1 = $07
//          ... same for $C583 / $C582 ...
//          LDY #$00 / LDA #$00 / STA (MB_BASEADDR),Y   ; ORB1 = $00 -> PB2 low
//                     LDA #$04 / STA (MB_BASEADDR),Y   ; ORB1 = $04 -> released
//          LDY #$80 / LDA #$00 / STA (MB_BASEADDR),Y   ; and on VIA2/AY2
//                     LDA #$04 / STA (MB_BASEADDR),Y
//
//      Test 2 below emits that byte sequence verbatim. It also explains
//      why the symptom was intermittent rather than constant: demos that
//      silence by writing 0 to R8/R9/R10 instead — CCII_2016's
//      `RESETMOCKIN` does exactly that — were never affected, because
//      those are ordinary stamped register writes.
//
// Both are pinned end-to-end on the rendered signal, because both were
// invisible to every register-level assertion: the CPU-side bank was
// correct in each case, and only the audio thread's copy diverged.

#include "M6502.h"
#include "Memory.h"
#include "Mockingboard.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr uint32_t kSr = 44100;
// PAL — DIX and the rest of the French Touch corpus, and the standard the
// replay cursor's target lag is sized in.
constexpr double kPalClockHz  = 1015625.0;
constexpr int    kFrameCycles = 20313;      // one PAL video frame
constexpr double kFrameSamples = 882.0;     // 44100 / 50
constexpr int    kChunk       = 256;        // AudioDevice's real period

constexpr uint8_t kPbInactive = 0x04;   // {BC1=0, BDIR=0, /RESET=1}
constexpr uint8_t kPbWrite    = 0x06;
constexpr uint8_t kPbLatch    = 0x07;
constexpr uint8_t kPbReset    = 0x00;   // /RESET asserted (PB2 low)

void writeVia(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    card.slotRomWrite(static_cast<uint8_t>(base | (reg & 0x0F)), v);
}

void ayWrite(MockingboardCard& card, int chip, uint8_t reg, uint8_t v)
{
    writeVia(card, chip, 0x03, 0xFF);         // DDRA = $FF
    writeVia(card, chip, 0x02, 0x07);         // DDRB = PB0..PB2 out
    writeVia(card, chip, 0x01, reg);          // ORA = reg
    writeVia(card, chip, 0x00, kPbLatch);
    writeVia(card, chip, 0x00, kPbInactive);
    writeVia(card, chip, 0x01, v);            // ORA = value
    writeVia(card, chip, 0x00, kPbWrite);
    writeVia(card, chip, 0x00, kPbInactive);
}

// Advance BOTH the memory cycle counter and the card, the way
// `Memory::advanceCycles` dispatches to slot peripherals. Driving only
// `mem` leaves `lastSyncCycle_` frozen between writes, which makes the
// card look permanently idle to the replay cursor — an artefact no real
// run has, and one that hides exactly the defects below.
void advance(Memory& mem, MockingboardCard& card, int cycles)
{
    mem.advanceCycles(cycles);
    card.advanceCycles(cycles);
}

double rms(const std::vector<float>& v, size_t from, size_t to)
{
    if (to > v.size()) to = v.size();
    if (from >= to) return 0.0;
    double s = 0.0;
    for (size_t i = from; i < to; ++i)
        s += static_cast<double>(v[i]) * static_cast<double>(v[i]);
    return std::sqrt(s / static_cast<double>(to - from));
}

// ─── A steady producer: volume-register PWM on channel A ────────────────
// Tone and noise masked in R7, so the channel level IS the amplitude
// register and the rendered waveform is exactly the square the CPU wrote.
// Same technique as `mockingboard_audio_quality`'s test 3, chosen here
// because its RMS is a direct read-out of whether the queue is flowing.
struct PwmDriver {
    static constexpr int kHalfPeriodCycles = 1000;
    int  cyclesIntoHalf = 0;
    bool high           = false;

    void arm(MockingboardCard& card) {
        ayWrite(card, 0, 7, 0x3F);
        ayWrite(card, 0, 8, 0x00);
    }
    // Run one video frame of production, toggling R8 on schedule.
    void frame(Memory& mem, MockingboardCard& card) {
        int remaining = kFrameCycles;
        while (remaining > 0) {
            const int step =
                std::min(remaining, kHalfPeriodCycles - cyclesIntoHalf);
            advance(mem, card, step);
            cyclesIntoHalf += step;
            remaining      -= step;
            if (cyclesIntoHalf >= kHalfPeriodCycles) {
                cyclesIntoHalf = 0;
                high = !high;
                ayWrite(card, 0, 8, high ? 0x0F : 0x00);
            }
        }
    }
};

// ─── Test 1: a rewind must not stall the replay queue ───────────────────
// `kind` 0 = the cycle counter is rolled back and nothing else (the CLI
// `--rewind`, `AiControlServer`, a card plugged after the snapshot was
// taken); 1 = the full `MachineSnapshot` path, which restores the card's
// own blob in the same pass that calls `mem.setCycleCounter`. The two
// reach the card through different doors — `syncToCpuCycleAt`'s backward
// -jump detector and `loadSnapshotState` — and both must break the
// timeline.
void testRewindDoesNotStallReplay(double rewindSeconds, int kind)
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);
    card.setCpuClock(kPalClockHz);
    // The machine has been up for a minute, so a deep rewind lands where
    // it was asked to instead of clamping at cycle 0 — which takes a
    // completely different (and accidentally harmless) path through the
    // cursor's re-anchor.
    mem.setCycleCounter(static_cast<uint64_t>(60.0 * kPalClockHz));

    PwmDriver drv;
    drv.arm(card);

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> chunk(kChunk), all;
    double samplesOwed = 0.0;
    size_t rewindAt = 0;

    constexpr int kFrames      = 260;    // 5.2 s
    constexpr int kRewindFrame = 100;    // 2.0 s in
    std::vector<uint8_t> blob;

    for (int f = 0; f < kFrames; ++f) {
        if (f == kRewindFrame) {
            rewindAt = all.size();
            const uint64_t now  = mem.getCycleCounter();
            const uint64_t back =
                static_cast<uint64_t>(rewindSeconds * kPalClockHz);
            if (kind == 1) {
                // What RewindBuffer / MachineSnapshot actually do: restore
                // the card's register blob, then re-anchor the clock.
                card.appendSnapshotState(blob);
                card.loadSnapshotState(blob.data(), blob.size());
            }
            mem.setCycleCounter(now > back ? now - back : 0);
        }
        drv.frame(mem, card);
        samplesOwed += kFrameSamples;
        while (samplesOwed >= kChunk) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
            samplesOwed -= kChunk;
        }
    }

    const double pre = rms(all, rewindAt - 8192, rewindAt);
    assert(pre > 0.02 && "producer was not audible before the rewind");

    // Worst RMS over any 1024-sample window in the second following the
    // rewind, skipping the first four windows: the jitter buffer is
    // allowed to refill its target lag, which is two PAL frames = 40 ms.
    constexpr size_t kWin  = 1024;
    constexpr size_t kSkip = 4 * kWin;                  // 93 ms
    double worst = 1e9;
    for (size_t i = rewindAt + kSkip; i + kWin < rewindAt + kSkip + kSr;
         i += kWin) {
        worst = std::min(worst, rms(all, i, i + kWin));
    }
    std::printf("  rewind %5.2f s (%s): pre=%.4f  worst window in the "
                "following second = %.4f\n",
                rewindSeconds, (kind == 0) ? "clock only" : "full snapshot",
                pre, worst);
    // Before the timeline generation existed this was flat zero for the
    // whole rewind depth.
    assert(worst > pre * 0.5);
}

// ─── Test 2: silencing the board must actually silence it ───────────────
// The DIX "note hangs between demos" report. A pattern player dumps all
// 14 registers per video tick — so the queue always carries a full frame
// of backlog — and then the board is silenced and nothing else is
// written. The card must go quiet and stay quiet.
//
// `useCardReset` picks which of the two wipe paths does the silencing:
// false = the AY /RESET strobe (VIA port B bit 2 low), which is what a
// demo emits on its way out; true = `MockingboardCard::onReset`, which is
// F12 / cold boot / a profile switch.
void testSilencingStopsTheNote(bool useCardReset)
{
    Memory mem;
    M6502  cpu(&mem);
    MockingboardCard card(4);
    card.setCpu(&cpu);
    card.setSampleRate(kSr);
    card.setVolume(1.0f);
    card.setMuted(false);
    card.setCpuClock(kPalClockHz);

    // TP = $140 -> 1015625/8/(2*320) = 198 Hz on channel A, tone only.
    static const uint8_t kFrameRegs[14] = {
        0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x3E, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    AudioSource* src = card.audioSource();
    assert(src);
    std::vector<float> chunk(kChunk), all;
    double samplesOwed = 0.0;
    size_t silencedAt  = 0;

    constexpr int kFrames    = 200;
    constexpr int kStopFrame = 60;
    for (int f = 0; f < kFrames; ++f) {
        int remaining = kFrameCycles;
        if (f < kStopFrame) {
            for (uint8_t r = 0; r < 14; ++r) {
                advance(mem, card, 60);
                ayWrite(card, 0, r, kFrameRegs[r]);
                remaining -= 60;
            }
        }
        if (f == kStopFrame) {
            silencedAt = all.size();
            if (useCardReset) {
                card.onReset();
            } else {
                // DIX `loader.a RESET_AY`, verbatim: ORB = $00 (PB2 low =
                // /RESET asserted) then $04 (released). MAME's card takes
                // the same wiring through `m_ay1->reset_w()`
                // (`a2mockingboard.cpp:366`), and `ay8910_reset_ym` then
                // writes 0 to registers 0..13.
                writeVia(card, 0, 0x02, 0x07);        // DDRB = PB0..PB2 out
                writeVia(card, 0, 0x00, kPbReset);
                writeVia(card, 0, 0x00, kPbInactive);
            }
        }
        advance(mem, card, remaining);
        samplesOwed += kFrameSamples;
        while (samplesOwed >= kChunk) {
            src->fillAudioBuffer(chunk.data(), kChunk);
            all.insert(all.end(), chunk.begin(), chunk.end());
            samplesOwed -= kChunk;
        }
    }

    const double pre  = rms(all, silencedAt - 8192, silencedAt);
    const double tail = rms(all, all.size() - 8192, all.size());
    assert(pre > 0.02 && "the note was not audible before silencing");

    // How long the card took to go quiet. The replay cursor runs a
    // deliberate two-PAL-frame lag behind the producer, so the silence is
    // legitimately ~40 ms late — that is the reset landing at its true
    // cycle stamp, with the music before it intact.
    constexpr size_t kWin = 1024;
    size_t quiet = all.size();
    for (size_t i = silencedAt; i + kWin < all.size(); i += kWin) {
        if (rms(all, i, i + kWin) < pre * 0.05) { quiet = i; break; }
    }
    const double secs = static_cast<double>(quiet - silencedAt) / kSr;
    std::printf("  %-18s pre=%.4f  tail=%.4f  time to silence = %.3f s\n",
                useCardReset ? "card reset" : "AY /RESET strobe",
                pre, tail, secs);
    // Before the fix `tail` came back equal to `pre` and the card never
    // went quiet at all.
    assert(tail < pre * 0.02);
    assert(secs < 0.20);
}

}  // namespace

int main()
{
    for (double s : { 0.5, 2.0, 5.0 })
        for (int kind : { 0, 1 }) testRewindDoesNotStallReplay(s, kind);
    std::printf("rewind does not stall replay ... OK\n");
    testSilencingStopsTheNote(false);
    testSilencingStopsTheNote(true);
    std::printf("silencing stops the note ....... OK\n");
    std::printf("Mockingboard timeline test passed.\n");
    return 0;
}

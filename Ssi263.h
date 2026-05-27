// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Ssi263 — Silicon Systems Inc. SSI263A phoneme speech synthesiser.
// Shared between MockingboardCard (Sound II variant — 1 or 2 SSI263s
// at $Cs40 / $Cs60) and EchoPlusCard (standalone). The chip plays one
// of 62 phonemes for a duration determined by its rate/duration
// registers, then signals the host (A/!R bit going high in the
// readback + an IRQ edge per the configured mode).
//
// Ported in spirit from AppleWin `source/SSI263.cpp` (LGPL — POM2's
// own chip-emulation code is independent; only the protocol contract
// is shared, and the eventual phoneme PCM blob — if/when imported —
// would be a separately-licensed data import). MAME does NOT
// implement the SSI263 (verified 2026-05-27, no `ssi263*` file in
// `src/devices/sound`), so this is the only reference target.
//
// Register layout (5 registers, $00-$04 within the chip's window):
//
//   $00 DURPHON  bits 7:6 = mode (see SsiMode below)
//                bits 5:0 = phoneme code (0..63; 62 are defined)
//   $01 INFLECT  bits 7:0 = inflection value (high byte of pitch tweak)
//   $02 RATEINF  bits 7:4 = rate (playback speed)
//                bits 3:0 = inflection (low byte)
//   $03 CTTRAMP  bit 7    = CTL (1 = power-down / silence; 0 = run)
//                bits 6:4 = articulation
//                bits 3:0 = amplitude
//   $04 FILFREQ  bits 7:0 = filter frequency (formant 4 cutoff)
//
// Reading any register returns a status byte with bit 7 = A/!R
// (Acknowledge / not Request) — high while the chip is requesting the
// next phoneme (i.e. the current one is done). The CPU clears A/!R by
// writing to one of $00..$02 (which also de-asserts the IRQ in the
// Mockingboard / Phasor wiring).
//
// IRQ wiring (host card decides):
//
//   * Mockingboard C: A/!R drives CA1 on VIA1. The VIA latches an
//     interrupt when CA1 sees the configured edge.
//   * Echo+ / Phasor speech: A/!R drives the slot IRQ line directly,
//     bypassing any 6522.
//
// `Ssi263::advance(cycles)` ticks the playback counter. When the
// configured phoneme duration elapses, A/!R goes 0 → 1 and `advance`
// returns `true` — the host card then takes the appropriate IRQ
// action (CA1 strobe vs direct slot IRQ).
//
// Audio status (v1 limitation)
// ----------------------------
// The chip ships **without phoneme PCM data** in this commit. The
// register state machine + IRQ timing are complete and pinned by
// tests, so games detect the chip and run their speech drivers
// correctly — they just don't make audible speech. The 62-phoneme
// PCM blob (~313 KB) lives in a separate commit so the user can
// explicitly decide whether to import AppleWin's data (LGPL — POM2
// currently has no LICENSE file, importing would effectively force
// LGPL on POM2 binaries) or regenerate via espeak.

#ifndef POM2_SSI263_H
#define POM2_SSI263_H

#include "CpuClock.h"

#include <cstddef>
#include <cstdint>

namespace pom2 {

class Ssi263
{
public:
    // ─── Register bit layouts ────────────────────────────────────────────
    enum : uint8_t {
        REG_DURPHON  = 0x0,
        REG_INFLECT  = 0x1,
        REG_RATEINF  = 0x2,
        REG_CTTRAMP  = 0x3,
        REG_FILFREQ  = 0x4,
    };

    // DURPHON bit decode.
    static constexpr uint8_t DURATION_MODE_MASK  = 0xC0;
    static constexpr uint8_t DURATION_MODE_SHIFT = 6;
    static constexpr uint8_t PHONEME_MASK        = 0x3F;

    // Mode bits (DR1:0) — bits 7:6 of DURPHON, after shift.
    enum SsiMode : uint8_t {
        MODE_IRQ_DISABLED                    = 0x0, // 00 — A/!R inactive, silent IRQ
        MODE_FRAME_IMMEDIATE_INFLECTION      = 0x1, // 01
        MODE_PHONEME_IMMEDIATE_INFLECTION    = 0x2, // 10
        MODE_PHONEME_TRANSITIONED_INFLECTION = 0x3, // 11
    };

    // RATEINF bit decode.
    static constexpr uint8_t RATE_MASK         = 0xF0;
    static constexpr uint8_t INFLECTION_LOW    = 0x0F;

    // CTTRAMP bit decode.
    static constexpr uint8_t CONTROL_MASK      = 0x80;
    static constexpr uint8_t ARTICULATION_MASK = 0x70;
    static constexpr uint8_t AMPLITUDE_MASK    = 0x0F;

    // FILFREQ sentinel for "silence" — when host writes 0xFF the chip
    // outputs silence regardless of phoneme.
    static constexpr uint8_t FILTER_FREQ_SILENCE = 0xFF;

    // ─── State accessors ─────────────────────────────────────────────────
    void reset();

    /// Read a register. Side effect: returns current status byte with
    /// bit 7 = A/!R. Mockingboard-faithful: only register $03 (or any
    /// — AppleWin treats reads uniformly) returns the status; in POM2
    /// every register read sees the A/!R bit in the high position.
    uint8_t read(uint8_t reg) const;

    /// Write a register. Side effects:
    ///   - Writes to $00..$02 de-assert IRQ + clear A/!R (CPU
    ///     acknowledged the request).
    ///   - Write to $00 (DURPHON) starts a new phoneme: latch the
    ///     mode + phoneme code, compute the duration in cycles from
    ///     the current rate + duration mode, arm the playback timer.
    ///   - Write to $03 (CTTRAMP) toggles CTL: setting bit 7 puts
    ///     the chip in power-down (silences audio + de-asserts IRQ);
    ///     clearing bit 7 (H→L transition) exits power-down and
    ///     starts the loaded phoneme.
    /// Returns true if A/!R was just cleared (caller may need to
    /// drop the IRQ line in that case).
    bool write(uint8_t reg, uint8_t val);

    /// Advance the playback counter by `cycles` 1.0227 MHz ticks.
    /// Returns true if A/!R transitioned 0 → 1 this slice (caller
    /// fires the IRQ edge).
    bool advance(int cycles);

    // ─── Readable state ──────────────────────────────────────────────────
    bool    aRequest()       const { return aRequest_; }
    bool    irqEnabled()     const { return ((durPhon_ & DURATION_MODE_MASK) >> DURATION_MODE_SHIFT)
                                                                   != MODE_IRQ_DISABLED; }
    bool    powerDown()      const { return (cttrAmp_ & CONTROL_MASK) != 0; }
    uint8_t currentPhoneme() const { return durPhon_ & PHONEME_MASK; }
    SsiMode currentMode()    const { return static_cast<SsiMode>(
                                       (durPhon_ & DURATION_MODE_MASK) >> DURATION_MODE_SHIFT); }
    uint8_t amplitude()      const { return cttrAmp_ & AMPLITUDE_MASK; }
    uint8_t articulation()   const { return (cttrAmp_ & ARTICULATION_MASK) >> 4; }
    uint8_t rate()           const { return (rateInf_ & RATE_MASK) >> 4; }
    int     phonemeRemainingCycles() const { return phonemeRemainingCycles_; }

    /// Direct peek into a register (no A/!R overlay) — for the UI panel.
    uint8_t peekRegister(uint8_t reg) const;

    /// Total number of phonemes accepted via writes to $00 since reset.
    /// Surfaces in the UI panel as a heartbeat for "the speech driver
    /// is actively pushing data".
    uint32_t phonemeWriteCount() const { return phonemeWriteCount_; }

    // ─── Audio render ─────────────────────────────────────────────────────
    /// Sum the chip's current audio output into `output` (does NOT
    /// overwrite). Pulls samples from the 62-phoneme PCM blob in
    /// `Ssi263PhonemeData.cpp` (ported from AppleWin), resampling
    /// from the chip's native 22050 Hz to `sampleRate` and scaling
    /// by the current amplitude register (R3 bits 3:0).
    ///
    /// State (the audio thread's playback cursor) lives in mutable
    /// members so the call signature stays simple; the host card's
    /// mutex serialises both audio-thread reads and CPU-thread
    /// register writes.
    void fillAudio(float* output, int frameCount, uint32_t sampleRate);

private:
    // Latched register values (last write).
    uint8_t durPhon_ = 0;
    uint8_t inflect_ = 0;
    uint8_t rateInf_ = 0;
    uint8_t cttrAmp_ = 0;
    uint8_t filFreq_ = 0;

    // Playback timing — countdown in CPU cycles to the end of the
    // current phoneme. Zero = not playing / phoneme finished.
    int phonemeRemainingCycles_ = 0;

    // A/!R flag — high when the chip is requesting the next phoneme
    // (i.e. the current one is done OR the chip is in power-down).
    bool aRequest_ = false;

    uint32_t phonemeWriteCount_ = 0;

    // ── Audio playback state (touched by audio thread under host mutex)
    // Current phoneme being rendered + position into its PCM segment.
    // `playbackPhoneme_` is sticky — it follows the most recently
    // WRITTEN phoneme; when the chip is requesting next (aRequest=true)
    // we keep playing the same phoneme in a loop (datasheet behaviour
    // for the "auto-rate, no inflection" modes).
    int    playbackPhoneme_ = 0;
    size_t playbackOffset_  = 0;
    // Resampler accumulator (host-rate fraction of a source-rate sample).
    float  resampleAccum_   = 0.0f;

    /// Compute the phoneme duration in CPU cycles based on the current
    /// `durPhon_` (mode + phoneme) and `rateInf_` (rate). AppleWin's
    /// formula (SSI263.cpp ~line 290):
    ///   ms = (((16 - (rate>>4)) * 4096) / 1023) * (4 - (dur>>6))
    /// converted to cycles via POM2_CPU_CLOCK_HZ.
    int computePhonemeDurationCycles() const;
};

} // namespace pom2

#endif // POM2_SSI263_H

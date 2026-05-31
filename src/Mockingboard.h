// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MockingboardCard — Sweet Microsystems Mockingboard A/C-compatible sound
// card. Two 6522 VIAs, each driving an AY-3-8910 PSG (3 tone channels +
// noise + envelope). The cards plug into any free slot (4 by convention);
// most software hard-codes slot 4. Ported in spirit from MAME
// `bus/a2bus/mockingboard.cpp` + `machine/6522via.cpp` + `sound/ay8910.cpp`,
// but trimmed to the subset music drivers and SFX engines actually use.
//
// Address map (all in the slot ROM window; the device-select range
// $C0(8+N)X is unused):
//
//   $Cn00..$Cn0F   VIA #1  (16 registers, partial decode — high bits 0..7
//                           mirror across $Cn10..$Cn7F)
//   $Cn80..$Cn8F   VIA #2  (16 registers, partial decode — mirrors across
//                           $Cn90..$CnFF)
//
// 6522 → AY wiring (Sweet Microsystems Mockingboard A/C schematic;
// AppleWin `Mockingboard.cpp:193` matches MAME `mockingboard.cpp`):
//
//   VIA Port A (8 bits)  → AY data bus  D0..D7
//   VIA Port B bit 0     → AY BC1
//   VIA Port B bit 1     → AY BDIR
//   VIA Port B bit 2     → AY !RESET (active low: PB2=0 zeroes AY regs)
//
// (Earlier POM2 had PB0=/RESET and PB2=BC1 — every INACTIVE strobe a
// music driver emitted between LATCH and WRITE looked like /RESET-
// asserted and wiped the AY bank, silencing Nox Archaist, Ultima IV
// and Total Replay. Fixed 2026-05-14.)
//
// Control sequence (BDIR, BC1):
//
//     00  inactive   (no bus action)
//     01  read       (rare — drivers don't usually read AY back)
//     10  write      (write Port A to the latched register)
//     11  latch addr (latch Port A as the next register address)
//
// IRQ routing. Each VIA's IRQ line (IFR.bit7) is OR'd onto the slot IRQ.
// The Mockingboard music driver convention is to clock VIA #1 timer 1
// (T1) in continuous mode at the music tick rate (~50 Hz, 25 Hz or some
// multiple) and update AY registers from the IRQ handler. This makes T1
// the single most important VIA feature — the timer + IFR + IER subset
// is what we model precisely; CB1/CB2/SR/PCR are stubbed.
//
// Audio path. The card owns an inner AudioSource that AudioDevice mixes
// into the same float32 mono buffer as the speaker / cassette. The audio
// callback runs on miniaudio's thread; it grabs a brief snapshot of both
// AY register banks under a mutex, releases, then synthesises n samples
// using its own (audio-thread-resident) tone / noise / envelope state.
// AY1 and AY2 are summed to mono — Mockingboard hardware does true stereo
// (one PSG per channel), but POM2's mixer is mono-only and the loss is
// audibly minor for the 3-voice arpeggios most period software produces.
//
// What's NOT modelled (deliberate, scope-bounded omissions):
//
//   * 6522 timer 2 (T2). Some demos use it for one-shot timing; Mocking-
//     board music drivers don't.
//   * 6522 shift register (SR). No software in the wild uses it on a
//     Mockingboard.
//   * 6522 CA1/CA2/CB1/CB2 control bits in PCR. Forwarded as-is on
//     read/write; their handshake side-effects are not modelled.
//   * AY-3-8910 I/O ports A/B (R14/R15). Mockingboard wires them as
//     unused; some Phasor / SuperMusicSynth boards use them for
//     channel routing but those are out of scope here.

#ifndef POM2_MOCKINGBOARD_H
#define POM2_MOCKINGBOARD_H

#include "Ay3_8910.h"
#include "AudioDevice.h"
#include "SlotPeripheral.h"
#include "Ssi263.h"
#include "Via6522.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

class M6502;

class MockingboardCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    /// Hardware variant:
    ///   AC      = vanilla Mockingboard A or C (2× 6522 + 2× AY-3-8910, no
    ///             speech). Slot ROM is just the two VIAs with partial
    ///             address decode mirroring.
    ///   SoundII = Mockingboard "C" / Sound II — adds an SSI263A speech
    ///             synth at $C(s)40-$C(s)44. The SSI263's A/!R signal
    ///             wires (inverted) to VIA1.CA1, so a phoneme-end edge
    ///             latches IFR.CA1 in VIA1 and (if IER.CA1 is enabled by
    ///             the host) drives the slot IRQ. Stock Sound II software
    ///             configures PCR.0 = 0 (negative-edge active) to match
    ///             the inverted wiring.
    enum class Variant { AC, SoundII };

    explicit MockingboardCard(int slot = kDefaultSlot, Variant variant = Variant::AC);
    ~MockingboardCard() override;

    Variant getVariant() const { return variant_; }
    bool    hasSsi263()  const { return ssi_ != nullptr; }

    int getSlot() const { return slot_; }

    /// Inject the host CPU. Used for the lazy-sync timer back-channel
    /// (`getCycleCountNow()` — VIA T1/T2 counters need sub-instruction
    /// catch-up between MMIO touches; see `syncToCpuCycle()`). IRQ
    /// routing does NOT go through this pointer — `SlotPeripheral::
    /// assertIrq()` fans out via SlotBus's installed router. Safe to
    /// leave null in headless tests (the card falls back to legacy
    /// batched timer advance).
    void setCpu(M6502* cpu) { cpu_ = cpu; }

    /// Pointer to the inner AudioSource. The caller (MainWindow) is
    /// responsible for registering / deregistering it with AudioDevice
    /// and for keeping it alive past the last audio callback — the
    /// AudioSource lives inside the card, so it must be removed from
    /// AudioDevice before the card is destroyed.
    AudioSource* audioSource();

    /// Audio output sample rate negotiated with the OS device. Default
    /// is AudioDevice::kSampleRate; override before plugging if your
    /// device picked a different rate (Apple Silicon often picks 48 kHz).
    void setSampleRate(uint32_t hz);

    /// Volume in [0, 2]. UI thread sets, audio thread reads.
    void  setVolume(float v);
    float getVolume() const;

    void setMuted(bool m);
    bool isMuted() const;

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Mockingboard"; }
    uint8_t slotRomRead (uint8_t low8) override;
    void    slotRomWrite(uint8_t low8, uint8_t v) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;
    void    onUnplug() override;
    // Rewind/snapshot: VIA + AY register/timer state (the audible music
    // state). SSI263 speech state is NOT captured (rare during a rewind).
    void    appendSnapshotState(std::vector<uint8_t>& out) const override;
    void    loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Test hooks ──────────────────────────────────────────────────────
    /// Direct read of an AY register (peeks the latched register bank).
    /// Test-only — production code goes through the VIA control sequence.
    uint8_t getAyRegister(int chip, int reg) const;
    /// Direct read of a VIA register without any side effect (no T1 IFR
    /// clear, no shift). Test-only.
    uint8_t peekViaRegister(int chip, int reg) const;
    /// Whether the slot IRQ line is currently asserted (cumulative across
    /// both VIAs). Test-only — forwards to the base-class state cached by
    /// `SlotPeripheral::assertIrq`.
    bool isIrqAsserted() const { return slotIrqAsserted(); }

    /// Telemetry for the Mockingboard UI panel: how many MMIO writes the
    /// card has accepted on each chip's VIA, and how many of those
    /// progressed to an AY-3-8910 register write (the second number
    /// stays 0 if the music driver is running but never completes a
    /// LATCH→WRITE strobe, which would point at a bus-protocol bug).
    /// Cleared on `onReset()`.
    uint32_t getViaWriteCount(int chip) const {
        return (chip == 0 || chip == 1) ? viaWriteCount_[chip] : 0;
    }
    uint32_t getAyWriteCount(int chip) const {
        return (chip == 0 || chip == 1) ? ayWriteCount_[chip] : 0;
    }
    uint32_t getAyResetCount(int chip) const {
        return (chip == 0 || chip == 1) ? ayResetCount_[chip] : 0;
    }

    /// Snapshot the SSI263 state for the UI panel. Returns false if this
    /// card variant has no SSI263 (vanilla AC). When true, `*out` is
    /// populated with the chip's current register banks + playback flags.
    struct Ssi263Snap {
        uint8_t  regs[5];
        uint8_t  currentPhoneme;
        uint8_t  mode;
        bool     aRequest;
        bool     powerDown;
        bool     irqEnabled;
        int      phonemeRemainingCycles;
        uint32_t phonemeWriteCount;
    };
    bool snapshotSsi263(Ssi263Snap* out) const;
    /// Per-AY-command transition counters. `cmd` is the 2-bit
    /// {BDIR,BC1} encoding: 0=INACTIVE, 1=READ, 2=WRITE, 3=LATCH.
    /// Returning 0 for an out-of-range `chip` or `cmd` keeps the
    /// diagnostic panel safe.
    uint32_t getAyCommandCount(int chip, int cmd) const;

private:
    // Forward declarations. `Via6522` and `Ay3_8910` are shared with
    // PhasorCard (see `Via6522.h` / `Ay3_8910.h`); only AudioSrc remains
    // private to this card.
    struct AudioSrc;

    int     slot_;
    Variant variant_ = Variant::AC;
    M6502*  cpu_ = nullptr;

    // VIAs and AYs — each VIA drives the AY at the same index. Held by
    // unique_ptr so the inner types can stay opaque in this header.
    std::unique_ptr<pom2::Via6522>  via_[2];
    std::unique_ptr<pom2::Ay3_8910> ay_[2];
    // Optional SSI263 speech synth — non-null only on Variant::SoundII.
    // Lives at slot ROM offsets $40-$4F (5 SSI263 regs + 11 mirrors).
    std::unique_ptr<pom2::Ssi263>   ssi_;
    std::unique_ptr<AudioSrc>       audio_;

    // Combined slot IRQ state — `via_[0].irqOut() || via_[1].irqOut()`.
    // Edge debouncing lives in SlotPeripheral::assertIrq, so no local
    // cache here.

    // Cycle the VIA timers were last advanced to. Both `slotRomRead/Write`
    // and the externally driven `advanceCycles()` catch up to "now"
    // (`cpu_->getCycleCountNow()`) before touching state, so a detection
    // routine that writes T1 and reads IFR a few cycles later sees the
    // up-to-date IFR instead of the stale batch-slice value. See the
    // CLAUDE.md "Mockingboard" section for the Nox Archaist / Skyfox /
    // Broadside detection-failure class this fixes.
    uint64_t lastSyncCycle_ = 0;
    void syncToCpuCycle();
    void syncToCpuCycleAt(uint64_t now);

    // Telemetry counters, bumped by slotRomWrite / onViaPortBChange.
    // Read by the Mockingboard ImGui panel; never affect emulation
    // semantics. Reset by onReset().
    uint32_t viaWriteCount_[2] = {0, 0};
    uint32_t ayWriteCount_[2]  = {0, 0};
    uint32_t ayResetCount_[2]  = {0, 0};
    // Bumped on every write to R13 (envelope shape) so the audio thread can
    // restart the envelope even when the shape value is unchanged.
    uint32_t ayEnvWriteCount_[2] = {0, 0};

    // Cross-thread guard. CPU thread takes it for VIA reads/writes and
    // for `advanceCycles`; audio thread takes it briefly to snapshot
    // the AY register banks. `mutable` so the test peek accessors can
    // be const.
    mutable std::mutex mtx;

    // Updates AY[i] in response to a VIA[i] Port B / Port A change.
    void onViaPortBChange(int chip);

    // Re-evaluates slot IRQ (OR of both VIAs) and forwards to the CPU.
    void updateIrq();
};

#endif // POM2_MOCKINGBOARD_H

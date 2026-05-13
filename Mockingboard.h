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
// 6522 → AY wiring (per Mockingboard schematic):
//
//   VIA Port A (8 bits)  → AY data bus  D0..D7
//   VIA Port B bit 0     → AY !RESET (active low: PB0=0 zeroes AY regs)
//   VIA Port B bit 1     → AY BDIR
//   VIA Port B bit 2     → AY BC1
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

#include "AudioDevice.h"
#include "SlotPeripheral.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

class M6502;

class MockingboardCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    explicit MockingboardCard(int slot = kDefaultSlot);
    ~MockingboardCard() override;

    int getSlot() const { return slot_; }

    /// Inject the host CPU so VIA T1 IRQs can drive the 6502 IRQ line.
    /// Safe to leave null (the card still runs, just doesn't fire IRQs).
    void setCpuIrqLine(M6502* cpu) { cpu_ = cpu; }

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

    // ─── Test hooks ──────────────────────────────────────────────────────
    /// Direct read of an AY register (peeks the latched register bank).
    /// Test-only — production code goes through the VIA control sequence.
    uint8_t getAyRegister(int chip, int reg) const;
    /// Direct read of a VIA register without any side effect (no T1 IFR
    /// clear, no shift). Test-only.
    uint8_t peekViaRegister(int chip, int reg) const;
    /// Whether the slot IRQ line is currently asserted (cumulative across
    /// both VIAs). Test-only.
    bool isIrqAsserted() const { return irqAsserted_; }

private:
    // Forward declarations of internal subdevices. Definitions live in
    // Mockingboard.cpp so the header stays light.
    struct Via6522;
    struct Ay3_8910;
    struct AudioSrc;

    int slot_;
    M6502* cpu_ = nullptr;

    // VIAs and AYs — each VIA drives the AY at the same index. Held by
    // unique_ptr so the inner types can stay opaque in this header.
    std::unique_ptr<Via6522>  via_[2];
    std::unique_ptr<Ay3_8910> ay_[2];
    std::unique_ptr<AudioSrc> audio_;

    // Combined slot IRQ state — `via_[0].irqOut() || via_[1].irqOut()`.
    // Cached so we only call `cpu_->setIRQ()` on transitions.
    bool irqAsserted_ = false;

    // Cycle the VIA timers were last advanced to. Both `slotRomRead/Write`
    // and the externally driven `advanceCycles()` catch up to "now"
    // (`cpu_->getCycleCountNow()`) before touching state, so a detection
    // routine that writes T1 and reads IFR a few cycles later sees the
    // up-to-date IFR instead of the stale batch-slice value. See the
    // CLAUDE.md "Mockingboard" section for the Nox Archaist / Skyfox /
    // Broadside detection-failure class this fixes.
    uint64_t lastSyncCycle_ = 0;
    void syncToCpuCycle();

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

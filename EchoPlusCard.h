// POM2 Apple II Emulator
// Copyright (C) 2026
//
// EchoPlusCard — Street Electronics Echo II / Echo+ standalone speech
// card. A single SSI263A phoneme synthesiser mapped at $Cs00..$Cs04
// (5 registers), with the chip's A/!R signal wired directly to the
// slot IRQ line. No 6522 in the basic Echo II — speech drivers poll
// the status register OR rely on the IRQ to know when the chip wants
// the next phoneme.
//
// Address map (s = slot number):
//
//   $Cs00..$Cs04   SSI263 registers (5)
//   $Cs05..$CsFF   open bus (returns $FF)
//   $C0(8+s)X      unused (device-select range — Echo II had no soft
//                  switches, unlike Phasor)
//
// Default slot: 4 (same as Mockingboard). Pluggable in any slot via
// Slot Configuration. Pairs naturally with a Mockingboard A/C at
// slot 4 + Echo+ at slot 2 (or vice versa) — the Apple II's standard
// "Mockingboard handles music, Echo+ handles speech" combo.
//
// Audio status (v1)
// -----------------
// The chip's register state machine + IRQ timing are complete (see
// `Ssi263.h`). Audio output is silent in this commit — the 62-phoneme
// PCM blob lives in a follow-up commit so the user can explicitly
// decide whether to import AppleWin's data (LGPL implications) or
// regenerate via espeak. Games that detect Echo+ via the SSI263
// register fingerprint will see the card and exercise their speech
// drivers correctly; they just won't speak yet.

#ifndef POM2_ECHO_PLUS_CARD_H
#define POM2_ECHO_PLUS_CARD_H

#include "AudioDevice.h"
#include "SlotPeripheral.h"
#include "Ssi263.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>

class M6502;

class EchoPlusCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    explicit EchoPlusCard(int slot = kDefaultSlot);
    ~EchoPlusCard() override;

    int getSlot() const { return slot_; }

    void setCpu(M6502* cpu) { cpu_ = cpu; }

    /// Inner AudioSource (silent v1 — see header). The caller registers
    /// it with AudioDevice the same way Mockingboard / Phasor do.
    AudioSource* audioSource();

    void  setSampleRate(uint32_t hz);
    void  setVolume(float v);
    float getVolume() const;
    void  setMuted(bool m);
    bool  isMuted() const;

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Echo+"; }
    uint8_t slotRomRead  (uint8_t low8) override;
    void    slotRomWrite (uint8_t low8, uint8_t v) override;
    void    advanceCycles(int cycles) override;
    void    onReset()  override;
    void    onUnplug() override;

    // ─── Test / UI hooks ─────────────────────────────────────────────────
    /// Snapshot of the SSI263 chip state (one-time copy under the
    /// parent mutex — the panel/test should call this rather than
    /// reaching into `chip()` directly).
    struct ChipSnap {
        uint8_t  regs[5];           // DURPHON / INFLECT / RATEINF / CTTRAMP / FILFREQ
        uint8_t  currentPhoneme;
        uint8_t  mode;              // bits 1:0 = SsiMode value
        bool     aRequest;
        bool     powerDown;
        bool     irqEnabled;
        int      phonemeRemainingCycles;
        uint32_t phonemeWriteCount;
    };
    ChipSnap snapshotChip() const;

    bool isIrqAsserted() const { return slotIrqAsserted(); }

private:
    struct AudioSrc;

    int    slot_;
    M6502* cpu_ = nullptr;

    pom2::Ssi263                ssi_;
    std::unique_ptr<AudioSrc>   audio_;

    mutable std::mutex mtx_;

    /// SSI263 advance — track cycles since last advance and forward
    /// them to the chip. Echo+ has no VIA so there's no separate timer
    /// to lazy-sync.
    void updateIrqFromChip();
};

#endif // POM2_ECHO_PLUS_CARD_H

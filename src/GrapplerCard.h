// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// GrapplerCard — Orange Micro Grappler+ parallel printer interface.
//
// What it adds over the synthetic `PrinterCard`
// ---------------------------------------------
// * A real 4 KB ROM dump (markadev/AppleII-RevEng/Orange-Micro-Grappler+).
//   The first 256 B sit at $CnXX (slot ROM); the full 4 KB is mirrored
//   into the shared expansion-ROM window at $C800-$CFFF so Grappler-aware
//   software detects the card via its ROM fingerprint.
// * MAME-faithful $C0(8+s)X register decode (`a2bus_grapplerplus_device`,
//   MAME `bus/a2bus/grappler.cpp`): data latch + strobe on offsets with
//   `!(offset & 3)` ($0/$4/$8/$C — spooled to the host buffer), A0 selects
//   the high 2 KB ROM bank at $C800, A1/A2 disable/enable the ACK IRQ.
//   Status reads return IRQ|DIP|BUSY|PE|SELECT|ACK — the synthetic printer
//   is always ready (BUSY=0, PE=0, SELECT=1) and ACKs instantly. Bytes the
//   Grappler firmware emits for its graphic-dump commands (^I G / ^I H)
//   are captured verbatim into the spool exactly as a real Epson-class
//   printer would have seen them. (An earlier revision spooled offset 1
//   only — on real hardware that's the BANK SELECT, so the genuine
//   firmware's `STA $C0n0` output was silently dropped and its status
//   polls read $FF = busy + paper out.)
// * Catalog key `"grappler"`. Defaults to slot 1 (the DOS / ProDOS
//   printer-scan slot).
//
// ROM-gated
// ---------
// Requires `roms/grappler_plus.bin` (4 KB). Without the dump the card
// still plugs but its slot ROM is a tiny stub — only the PR#n
// trampoline + the always-ready data port work; software that fingerprints
// the Grappler ROM (e.g. AppleWorks's "Printer = Grappler+") sees a
// blank card. This mirrors the CFFA's ROM-gated approach.

#ifndef POM2_GRAPPLER_CARD_H
#define POM2_GRAPPLER_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class GrapplerCard : public SlotPeripheral
{
public:
    static constexpr int    kDefaultSlot = 1;
    static constexpr size_t kRomBytes    = 0x1000;   // 4 KB EPROM

    explicit GrapplerCard(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    // ─── ROM loading ─────────────────────────────────────────────────────
    /// Load the 4 KB Grappler+ ROM dump. Must be exactly 4096 bytes.
    /// Without it the card falls back to a minimal synthetic slot ROM
    /// that supports PR#n but lacks the graphics dump entry points.
    bool loadRom(const std::string& path);
    bool isRomLoaded() const { return romLoaded_; }
    const std::string& romSource() const { return romSource_; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Grappler+ (Orange Micro)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead     (uint8_t low8) override;
    uint8_t expansionRomRead(uint16_t offset) override;
    void    onReset() override;

    /// Rewind/snapshot hooks — the ROM bank / ACK latch / IRQ-enable
    /// flip-flops are guest-visible state (a rewound guest mid-print may
    /// expect the other $C800 bank or a pending ACK IRQ). The host-side
    /// spool is deliberately NOT rewound (it is an output log, like the
    /// PrinterCard's).
    void appendSnapshotState(std::vector<uint8_t>& out) const override;
    void loadSnapshotState(const uint8_t* data, std::size_t len) override;

    // ─── Spool access (shared shape with PrinterCard for UI reuse) ───────
    std::vector<uint8_t> spoolBytes() const;
    std::string          spoolText()  const;
    size_t               bytesWritten() const;
    void                 clearSpool();
    /// Streaming variant used by the host-side ImageWriter — see
    /// `PrinterCard::drainSpoolFrom`.
    size_t drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const;

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    std::array<uint8_t, 256>       stubRom_{};      // used until loadRom
    bool romLoaded_ = false;
    std::string romSource_;

    mutable std::mutex   bufferMtx_;
    std::vector<uint8_t> spool_;

    // ─── MAME a2bus_grapplerplus register state (grappler.cpp) ──────────
    bool romBankHigh_ = false;  // A0 write sets; any $CnXX read clears
    bool ackLatch_    = true;   // reset = 1 (MAME m_ack_latch); instant ACK
    bool irqDisable_  = true;   // A1 disables / A2 enables the ACK IRQ
    bool irqAsserted_ = false;

    void updateIrq();

    void buildStubRom();
};

#endif // POM2_GRAPPLER_CARD_H

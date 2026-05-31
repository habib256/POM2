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
// * A spool-only data port at $C0(8+s)1 — same host-side semantics as
//   `PrinterCard` (every byte is enqueued into a host buffer, no BUSY/STB
//   modelling). Bytes Grappler firmware emits for its graphic-dump
//   commands (^I G / ^I H) are captured verbatim into the spool exactly
//   as a real Epson-class printer would have seen them.
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
    void    onReset() override {}

    // ─── Spool access (shared shape with PrinterCard for UI reuse) ───────
    std::vector<uint8_t> spoolBytes() const;
    std::string          spoolText()  const;
    size_t               bytesWritten() const;
    void                 clearSpool();

private:
    int slot_;
    std::array<uint8_t, kRomBytes> rom_{};
    std::array<uint8_t, 256>       stubRom_{};      // used until loadRom
    bool romLoaded_ = false;
    std::string romSource_;

    mutable std::mutex   bufferMtx_;
    std::vector<uint8_t> spool_;

    void buildStubRom();
};

#endif // POM2_GRAPPLER_CARD_H

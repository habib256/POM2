// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrinterCard — synthetic Apple Parallel-style printer interface card.
// Captures every byte the Apple II "prints" through PR#n into a host-side
// spool buffer, which the UI saves to .txt / .pdf to match the macOS
// print-to-PDF workflow.
//
// Slot 1 by convention (the slot DOS / ProDOS / Pascal scan for the
// canonical printer device), but any slot 1-7 works; the slot ROM bakes
// the slot number into the CSWL/CSWH redirection and the device-select
// address inside the output trampoline.
//
// Wire model
// ----------
// One data port at $C0(8+s)1 — write = enqueue the byte to the spool;
// read = always 0xFF (printer permanently "ready"). All other device-
// select offsets read 0xFF / writes ignored.
//
// PR#n protocol (minimal — DOS/BASIC works, no Pascal driver needed)
// ------------------------------------------------------------------
// `PR#n` from Applesoft/Monitor JSRs to $Cn00. Our slot ROM hooks
// CSWL/CSWH ($36/$37) so subsequent COUT calls jump to our 3-byte
// output handler at $Cn31, which stores A into the data port and RTSes.
// A is preserved (STA doesn't clobber it), so callers that re-read A
// after COUT keep working.
//
// Slot ROM layout (s = slot number, slotHi = $C0+s):
//
//   $Cn00  4C 20 ss  JMP $Cn20                     (skip sig bytes)
//   $Cn05  38        Pascal 1.1 sig 1               (SEC)
//   $Cn07  18        Pascal 1.1 sig 2               (CLC)
//   $Cn0B  01        Pascal 1.1 firmware revision
//   $Cn0C  00        Pascal device class = printer
//   $Cn20  A9 31     LDA #$31                       (CSWL low byte)
//   $Cn22  85 36     STA $36                        (= low byte of $Cn31)
//   $Cn24  A9 ss     LDA #slotHi                    (CSWH = $C0+s)
//   $Cn26  85 37     STA $37
//   $Cn28  60        RTS
//   $Cn31  8D 91 c0  STA $C0(8+s)1                  (data port write)
//   $Cn34  60        RTS
//
// Everything else is $EA (NOP) padding so a sloppy probe falling
// through doesn't trip on stale bytes.
//
// Why synthetic (no PROM dump)
// ----------------------------
// The real Apple Parallel Printer Card ROM had hardware-specific
// strobe/busy bit-banging on its 6821 PIA that doesn't translate to a
// host-side spool. Following the `ProDOSHardDiskCard` model (DEV.md §
// "ProDOSHardDiskCard"), we ship a synthetic ROM whose only job is to
// thread COUT bytes into our C++ buffer — no MAME parity to chase
// (MAME `a2parprn` exists but emulates a real bit-banged port that
// can't be observed without a physical printer). PDF rendering and
// raw .txt dump live in the UI, not in the card.

#ifndef POM2_PRINTER_CARD_H
#define POM2_PRINTER_CARD_H

#include "SlotPeripheral.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class PrinterCard : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 1;

    explicit PrinterCard(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    // ─── SlotPeripheral overrides ────────────────────────────────────────
    std::string_view name() const override { return "Printer (Parallel)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override { return rom_[low8]; }
    void    onReset() override {}   // spool survives Ctrl-Reset (user clears)

    // ─── Spool access ────────────────────────────────────────────────────

    /// Raw spool bytes since the last clear. High bit NOT stripped — the
    /// caller decides what to do with Apple II bit-7-set text.
    std::vector<uint8_t> spoolBytes() const;

    /// Spool rendered as a text string: high bit stripped, CR ($0D) mapped
    /// to LF ($0A) so it displays naturally in a host text widget /
    /// .txt file. Form-feed ($0C) preserved as-is.
    std::string spoolText() const;

    /// Number of bytes written since the last clear.
    size_t bytesWritten() const;

    /// Clear the spool buffer. Called by the UI "Clear" button and after
    /// a successful "Save spool as…" if the user asks to start fresh.
    void clearSpool();

private:
    int slot_;
    std::array<uint8_t, 256> rom_{};

    // The spool is small enough (text, not images) that an unbounded
    // std::vector is fine — a 100-page report at 80 cols × 60 lines is
    // ~500 KB. A future hardening pass could cap it.
    mutable std::mutex bufferMtx_;
    std::vector<uint8_t> spool_;

    void buildRom();
};

#endif // POM2_PRINTER_CARD_H

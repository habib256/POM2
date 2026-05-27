// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrinterCard — see header for ROM layout + protocol notes.

#include "PrinterCard.h"

#include <initializer_list>

PrinterCard::PrinterCard(int slot) : slot_(slot)
{
    buildRom();
}

uint8_t PrinterCard::deviceSelectRead(uint8_t /*low4*/)
{
    // Synthetic always-ready printer. Real Apple Parallel Cards used a
    // BUSY/READY bit, but a host-side spool never blocks.
    return 0xFF;
}

void PrinterCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Data port at $C0(8+s)1; ignore all other writes (status / strobe
    // ports on the real card have no host-side meaning).
    if (low4 != 1) return;
    std::lock_guard<std::mutex> lk(bufferMtx_);
    spool_.push_back(v);
}

std::vector<uint8_t> PrinterCard::spoolBytes() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spool_;
}

std::string PrinterCard::spoolText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    std::string out;
    out.reserve(spool_.size());
    for (uint8_t b : spool_) {
        const uint8_t c = b & 0x7F;        // Apple II output sets bit 7
        if (c == 0x0D) out.push_back('\n'); // CR → LF for host display
        else if (c == 0x00) continue;       // drop NULs (paddle / strobe noise)
        else                out.push_back(static_cast<char>(c));
    }
    return out;
}

size_t PrinterCard::bytesWritten() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spool_.size();
}

void PrinterCard::clearSpool()
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    spool_.clear();
}

void PrinterCard::buildRom()
{
    rom_.fill(0xEA);     // NOP padding — see header for layout

    const uint8_t slotHi = static_cast<uint8_t>(0xC0 + slot_);
    const uint8_t dataLo = static_cast<uint8_t>(0x80 + slot_ * 16 + 1);

    auto putAt = [&](uint8_t addr, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) rom_[addr++] = b;
    };

    // PR#n entry at $Cn00 — jump past the Pascal signature region.
    putAt(0x00, { 0x4C, 0x20, slotHi });       // JMP $Cn20

    // Pascal 1.1 autodetect signature — ProDOS scans for these to publish
    // the card in its device list. Apple Pascal also recognises this
    // shape; we don't implement the full PINIT/PREAD/PWRITE/PSTATUS
    // entry table because BASIC PR#n is the only documented use case
    // for a printer card and Pascal printer drivers were rare in the
    // POM2 software corpus.
    rom_[0x05] = 0x38;     // SEC
    rom_[0x07] = 0x18;     // CLC
    rom_[0x0B] = 0x01;     // Pascal firmware revision
    rom_[0x0C] = 0x00;     // device class = printer

    // PR#n trampoline at $Cn20 — hook CSWL/CSWH ($36/$37) to point at our
    // output handler at $Cn31. The next COUT call lands there instead of
    // the standard screen routine.
    putAt(0x20, {
        0xA9, 0x31,            // LDA #$31           (low byte of $Cn31)
        0x85, 0x36,            // STA CSWL
        0xA9, slotHi,          // LDA #slotHi        (= $C0+s)
        0x85, 0x37,            // STA CSWH
        0x60                   // RTS
    });

    // Output handler at $Cn31 — write A to the data port; A is preserved
    // (STA does not modify it), satisfying COUT's "A unchanged on exit"
    // convention. The CPU's existing flags/X/Y are untouched.
    putAt(0x31, {
        0x8D, dataLo, 0xC0,    // STA $C0(8+s)1
        0x60                   // RTS
    });
}

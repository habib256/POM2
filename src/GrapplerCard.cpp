// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// GrapplerCard — see header for ROM layout + protocol notes.

#include "GrapplerCard.h"

#include "Logger.h"

#include <fstream>
#include <initializer_list>
#include <iterator>

GrapplerCard::GrapplerCard(int slot) : slot_(slot)
{
    buildStubRom();
}

bool GrapplerCard::loadRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        pom2::log().warn("Grappler", "Cannot open Grappler+ ROM: " + path);
        return false;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                                std::istreambuf_iterator<char>());
    if (bytes.size() != kRomBytes) {
        pom2::log().warn("Grappler",
            "Grappler+ ROM " + path + " has unexpected size " +
            std::to_string(bytes.size()) +
            " B (expected 4096) — using stub ROM");
        return false;
    }
    std::copy(bytes.begin(), bytes.end(), rom_.begin());
    romLoaded_ = true;
    romSource_ = path;
    pom2::log().info("Grappler", "Loaded Grappler+ ROM: " + path);
    return true;
}

uint8_t GrapplerCard::deviceSelectRead(uint8_t /*low4*/)
{
    // Synthetic always-ready printer (real Grappler+ exposes a busy/ack
    // status, but a host-side spool never blocks).
    return 0xFF;
}

void GrapplerCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Data port at $C0(8+s)1 — same convention as PrinterCard. Other
    // device-select offsets are ignored (no host-side meaning for the
    // strobe / ack lines).
    if (low4 != 1) return;
    std::lock_guard<std::mutex> lk(bufferMtx_);
    spool_.push_back(v);
}

uint8_t GrapplerCard::slotRomRead(uint8_t low8)
{
    if (romLoaded_) return rom_[low8];
    return stubRom_[low8];
}

uint8_t GrapplerCard::expansionRomRead(uint16_t offset)
{
    // The 4 KB Grappler EPROM is wired so the full chip is visible across
    // the shared $C800-$CFFF window. Only the first 2 KB are reachable
    // through the Apple II expansion window; the upper 2 KB are bank-
    // selectable on the real card via writes to $C0(8+s)X, which POM2
    // doesn't currently model. Stock PR# / status reads + the standard
    // graphics dump triggers live in the lower 2 KB, so this is enough
    // for software detection.
    if (!romLoaded_) return 0xFF;
    if (offset >= 0x800) return 0xFF;
    return rom_[offset];
}

std::vector<uint8_t> GrapplerCard::spoolBytes() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spool_;
}

std::string GrapplerCard::spoolText() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    std::string out;
    out.reserve(spool_.size());
    for (uint8_t b : spool_) {
        const uint8_t c = b & 0x7F;
        if (c == 0x0D) out.push_back('\n');
        else if (c == 0x00) continue;
        else                out.push_back(static_cast<char>(c));
    }
    return out;
}

size_t GrapplerCard::bytesWritten() const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    return spool_.size();
}

void GrapplerCard::clearSpool()
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    spool_.clear();
}

void GrapplerCard::buildStubRom()
{
    // Fallback ROM used until the user drops a Grappler+ dump in roms/.
    // Mirrors PrinterCard's PR#n trampoline so DOS/BASIC `PR#1` still
    // works; software that fingerprints the Grappler ROM sees an empty
    // card (matches "no Grappler installed" detection paths). Padding
    // is $EA (NOP) so a sloppy probe walking through doesn't hit garbage.
    stubRom_.fill(0xEA);
    const uint8_t slotHi = static_cast<uint8_t>(0xC0 + slot_);
    const uint8_t dataLo = static_cast<uint8_t>(0x80 + slot_ * 16 + 1);

    auto putAt = [&](uint8_t addr, std::initializer_list<uint8_t> bytes) {
        for (uint8_t b : bytes) stubRom_[addr++] = b;
    };

    // $Cn00: JMP $Cn20 (skip Pascal sig).
    putAt(0x00, { 0x4C, 0x20, slotHi });
    // Pascal 1.1 autodetect — same shape as PrinterCard.
    stubRom_[0x05] = 0x38;     // SEC
    stubRom_[0x07] = 0x18;     // CLC
    stubRom_[0x0B] = 0x01;     // Pascal firmware rev
    stubRom_[0x0C] = 0x00;     // device class = printer
    // PR#n CSWL/CSWH install.
    putAt(0x20, {
        0xA9, 0x31, 0x85, 0x36,
        0xA9, slotHi, 0x85, 0x37,
        0x60
    });
    // Output handler: STA $C0(8+s)1 / RTS.
    putAt(0x31, { 0x8D, dataLo, 0xC0, 0x60 });
}

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
    // MAME `a2bus_grapplerplus_device::read_c0nx` (grappler.cpp): every
    // device-select offset returns the same status byte —
    //   bit 7 IRQ pending · bits 6-4 DIP switches · bit 3 BUSY ·
    //   bit 2 PE (paper empty) · bit 1 SELECT · bit 0 ACK latch.
    // POM2's synthetic printer is permanently ready: BUSY=0, PE=0,
    // SELECT=1, ACK latched (instant ack), DIP = 000. The previous $FF
    // read decoded for real firmware as "busy AND out of paper" — the
    // worst possible value for its pre-byte status poll.
    return static_cast<uint8_t>((irqAsserted_ ? 0x80 : 0x00) |
                                0x02 |                    // SELECT high
                                (ackLatch_ ? 0x01 : 0x00));
}

void GrapplerCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // MAME `a2bus_grapplerplus_device::write_c0nx` (grappler.cpp):
    //   !(offset & 3)  → latch data + strobe (offsets $0/$4/$8/$C)
    //   A0 set         → select the high 2 KB ROM bank at $C800
    //   A1 set         → disable the ACK IRQ (and release a pending one)
    //   A2 set (¬A1)   → enable the ACK IRQ
    // The previous decode spooled offset 1 ONLY — on real hardware that's
    // the bank select, so the genuine firmware's `STA $C0n0` printed into
    // the void. The synthetic printer consumes the byte and ACKs
    // instantly, so the ACK latch is re-set on the spot.
    if (!(low4 & 0x03)) {
        {
            std::lock_guard<std::mutex> lk(bufferMtx_);
            spool_.push_back(v);
        }
        ackLatch_ = true;        // instant ACK pulse
        updateIrq();
    }
    if (low4 & 0x01) romBankHigh_ = true;
    if (low4 & 0x02) {
        irqDisable_ = true;
        updateIrq();
    } else if (low4 & 0x04) {
        irqDisable_ = false;
        updateIrq();
    }
}

uint8_t GrapplerCard::slotRomRead(uint8_t low8)
{
    if (romLoaded_) {
        // MAME `read_cnxx` side effects: any $CnXX read resets the ROM
        // bank to 0, and while the ACK latch is CLEAR, address bit 6 is
        // forced low for $Cn80-$CnFF fetches — the firmware senses ACK
        // through ROM reads (`m_rom[(!ack && BIT(offset,7)) ? offset&0xBF
        // : offset]`). With the instant-ACK printer the latch is almost
        // always set, but the wiring is kept faithful.
        romBankHigh_ = false;
        const uint8_t idx = (!ackLatch_ && (low8 & 0x80))
                                ? static_cast<uint8_t>(low8 & 0xBF)
                                : low8;
        return rom_[idx];
    }
    return stubRom_[low8];
}

uint8_t GrapplerCard::expansionRomRead(uint16_t offset)
{
    // The 4 KB Grappler EPROM appears in the shared $C800-$CFFF window
    // 2 KB at a time; A0 device-select writes choose the high bank and
    // any $CnXX read drops back to the low one (MAME `read_c800` /
    // `set_rom_bank`). Stock PR# / status entry points live in the low
    // bank; the graphics-dump code spills into the high one.
    if (!romLoaded_) return 0xFF;
    return rom_[(offset & 0x7FF) | (romBankHigh_ ? 0x800u : 0x000u)];
}

void GrapplerCard::onReset()
{
    // MAME reset_from_bus(): bank 0, ACK latch back to its inactive (set)
    // level, IRQ flip-flop disabled and the line released.
    romBankHigh_ = false;
    ackLatch_    = true;
    irqDisable_  = true;
    updateIrq();
}

void GrapplerCard::updateIrq()
{
    // MAME: the slot IRQ asserts while the ACK latch is set and the
    // enable flip-flop is on; only the A1 disable (or reset) releases it.
    const bool want = ackLatch_ && !irqDisable_;
    if (want != irqAsserted_) {
        irqAsserted_ = want;
        assertIrq(want);
    }
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

size_t GrapplerCard::drainSpoolFrom(size_t from, std::vector<uint8_t>& out) const
{
    std::lock_guard<std::mutex> lk(bufferMtx_);
    // `from` past the end means the spool was cleared behind the caller's
    // back (the panel's "Clear spool" button) — hand back everything so the
    // consumer resynchronises instead of silently going deaf.
    const size_t start = (from > spool_.size()) ? 0 : from;
    out.insert(out.end(), spool_.begin() + static_cast<std::ptrdiff_t>(start),
               spool_.end());
    return spool_.size();
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
    // Data port = $C0(8+s)0 — the MAME-faithful data latch offset
    // (!(offset & 3)). The stub used to print through offset 1, which on
    // the real card (and now in deviceSelectWrite) is the ROM bank select.
    const uint8_t dataLo = static_cast<uint8_t>(0x80 + slot_ * 16);

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
    // Output handler: STA $C0(8+s)0 / RTS.
    putAt(0x31, { 0x8D, dataLo, 0xC0, 0x60 });
}

void GrapplerCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.push_back('G'); out.push_back('P'); out.push_back(1);
    out.push_back(romBankHigh_ ? 1 : 0);
    out.push_back(ackLatch_    ? 1 : 0);
    out.push_back(irqDisable_  ? 1 : 0);
}

void GrapplerCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (len < 6 || data[0] != 'G' || data[1] != 'P' || data[2] != 1) return;
    romBankHigh_ = data[3] != 0;
    ackLatch_    = data[4] != 0;
    irqDisable_  = data[5] != 0;
    updateIrq();   // re-derive the slot IRQ line from the restored state
}

// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// EchoPlusTMS5220Card — see header for the chipset overview + scaffold
// status. Audio synth and the LPC core are deferred; this file ships the
// SlotPeripheral surface only.

#include "EchoPlusTMS5220Card.h"

#include "ByteIO.h"

#include <cstring>

EchoPlusTMS5220Card::EchoPlusTMS5220Card(int slot) : slot_(slot)
{
    onReset();
}

void EchoPlusTMS5220Card::onReset()
{
    std::lock_guard<std::mutex> lk(mtx_);
    // TMS5220 power-on / reset status. MAME `tms5220.cpp:894-907`
    // (status_read): status byte = TS<<7 | BL<<6 | BE<<5 (low 5 bits
    // open bus). Reset sets `m_buffer_empty = m_buffer_low = true`
    // (`tms5220.cpp:1771`), and the flag logic agrees: BL is active
    // whenever fifo_count <= 8 and BE whenever fifo_count == 0
    // (`update_fifo_status_and_ints`, `tms5220.cpp:754-790`) — an empty
    // FIFO is by definition also "low". So idle = TS=0, BL=1, BE=1 =
    // 0x60. (An earlier scaffold reported BE-only 0x20; a driver
    // FIFO-pacing on BL=1 "ready for the next byte" would then poll
    // $Cs00 forever.) Drivers that poll `LDA $Cs00 / BMI ...` still see
    // "not talking" (TS=0) and fall through their wait loops.
    tmsStatus_    = 0x60;          // BL=1 | BE=1, TS=0
    tmsLastWrite_ = 0x00;
    for (int i = 0; i < 2; ++i) {
        ay_[i].reset();
        aySelected_[i] = 0;
    }
}

uint8_t EchoPlusTMS5220Card::slotRomRead(uint8_t low8)
{
    std::lock_guard<std::mutex> lk(mtx_);
    switch (low8) {
    case 0x00:                      // TMS5220 status
        return tmsStatus_;
    case 0x01:                      // TMS5220 reset acknowledge — open bus
        return 0xFF;
    case 0x04:                      // AY-3-8913 #1 data read (R[selected])
    case 0x05:
        return ay_[0].regs[aySelected_[0] & 0x0F];
    case 0x06:                      // AY-3-8913 #2 data read
    case 0x07:
        return ay_[1].regs[aySelected_[1] & 0x0F];
    default:
        return 0xFF;
    }
}

void EchoPlusTMS5220Card::slotRomWrite(uint8_t low8, uint8_t v)
{
    std::lock_guard<std::mutex> lk(mtx_);
    switch (low8) {
    case 0x00:                      // TMS5220 data byte
        // Accept and drop — no LPC core yet. Track the last byte for the
        // diagnostic snapshot so a debug panel can confirm software is
        // pushing data at us.
        tmsLastWrite_ = v;
        break;
    case 0x01:                      // TMS5220 reset / stop
        tmsStatus_ = 0x60;          // back to idle: BL|BE set, TS clear
                                    // (MAME tms5220.cpp:1771 reset state)
        break;
    case 0x04:                      // AY-3-8913 #1 address latch (low nibble)
        aySelected_[0] = v & 0x0F;
        break;
    case 0x05:                      // AY-3-8913 #1 data write
        ay_[0].regs[aySelected_[0] & 0x0F] = v;
        break;
    case 0x06:                      // AY-3-8913 #2 address latch
        aySelected_[1] = v & 0x0F;
        break;
    case 0x07:                      // AY-3-8913 #2 data write
        ay_[1].regs[aySelected_[1] & 0x0F] = v;
        break;
    default:
        break;                      // open bus
    }
}

// ─── Rewind / snapshot state ────────────────────────────────────────────────
// Layout: 'E' 'T' 'S' ver(1) | tmsStatus | tmsLastWrite | aySel0 | aySel1 |
//         AY#1 (Ay3_8910::kSnapshotBytes) | AY#2. The AY blocks carry the full
// register bank the card exposes at $Cs04-$Cs07, so a restore puts back what a
// read-back would see. Same shape as MockingboardCard / PhasorCard: validate
// the whole length BEFORE touching a chip, so a short blob leaves the card
// untouched rather than half-restored.
namespace {
constexpr std::size_t kEchoTmsHeaderBytes = 8;
}

void EchoPlusTMS5220Card::appendSnapshotState(std::vector<uint8_t>& out) const
{
    using namespace pom2::byteio;
    std::lock_guard<std::mutex> lk(mtx_);
    putU8(out, 'E'); putU8(out, 'T'); putU8(out, 'S'); putU8(out, 1);
    putU8(out, tmsStatus_);
    putU8(out, tmsLastWrite_);
    putU8(out, aySelected_[0]);
    putU8(out, aySelected_[1]);
    ay_[0].appendSnapshot(out);
    ay_[1].appendSnapshot(out);
}

void EchoPlusTMS5220Card::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    pom2::byteio::Reader r(data, len);
    if (!r.has(kEchoTmsHeaderBytes)) return;
    if (r.u8() != 'E' || r.u8() != 'T' || r.u8() != 'S') return;
    if (r.u8() != 1) return;
    if (len < kEchoTmsHeaderBytes + 2 * pom2::Ay3_8910::kSnapshotBytes) return;
    tmsStatus_     = r.u8();
    tmsLastWrite_  = r.u8();
    aySelected_[0] = r.u8() & 0x0F;
    aySelected_[1] = r.u8() & 0x0F;
    for (int i = 0; i < 2; ++i) {
        ay_[i].loadSnapshot(r.p + r.pos);
        r.pos += pom2::Ay3_8910::kSnapshotBytes;
    }
}

EchoPlusTMS5220Card::Snap EchoPlusTMS5220Card::snapshot() const
{
    std::lock_guard<std::mutex> lk(mtx_);
    Snap s{};
    s.tmsStatus    = tmsStatus_;
    s.tmsLastWrite = tmsLastWrite_;
    for (int i = 0; i < 2; ++i)
        std::memcpy(s.ayRegs[i], ay_[i].regs, sizeof(ay_[i].regs));
    return s;
}

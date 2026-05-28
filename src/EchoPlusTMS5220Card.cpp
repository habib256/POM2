// POM2 Apple II Emulator
// Copyright (C) 2026
//
// EchoPlusTMS5220Card — see header for the chipset overview + scaffold
// status. Audio synth and the LPC core are deferred; this file ships the
// SlotPeripheral surface only.

#include "EchoPlusTMS5220Card.h"

#include <cstring>

EchoPlusTMS5220Card::EchoPlusTMS5220Card(int slot) : slot_(slot)
{
    onReset();
}

void EchoPlusTMS5220Card::onReset()
{
    std::lock_guard<std::mutex> lk(mtx_);
    // TMS5220 power-on status (rough approximation):
    //   bit 7 = TS (talking status) — 0 idle
    //   bit 6 = BL (buffer low)     — 0 idle, set by speech FIFO drain
    //   bit 5 = BE (buffer empty)   — 1 idle (FIFO empty on cold start)
    // Drivers that poll `LDA $Cs00 / BMI ...` see "not talking" and
    // proceed past the wait loop cleanly.
    tmsStatus_    = 0x20;          // BE=1, others 0
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
        tmsStatus_ = 0x20;          // back to "idle, FIFO empty"
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

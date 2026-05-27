// POM2 Apple II Emulator
// Copyright (C) 2026

#include "LeChatMauveCard.h"

void LeChatMauveCard::onReset()
{
    // Real Féline cards default to COL140 at power-up — the FIFO bits
    // are pulled to 1 by the Péritel output stage. AN3 powers up HIGH
    // (DHIRES off — matches MAME `apple2video device_reset` m_dhires=false),
    // so an3Prev starts TRUE: a bare $C05F right after reset is NOT a rising
    // edge and must not clock the FIFO. (A normal $C05E→$C05F pair still
    // produces exactly one shift; init=false admitted a spurious first shift.)
    fifo             = 0b11;
    mode             = RenderMode::COL140;
    an3Prev          = true;
    eightyColLatched = false;
}

void LeChatMauveCard::onVideoSoftSwitch(uint16_t addr)
{
    // Data line first — the FIFO samples 80COL on the AN3 rising edge,
    // so the data bit must be up-to-date before we look for the clock.
    if (addr == 0xC00C) { eightyColLatched = false; return; }
    if (addr == 0xC00D) { eightyColLatched = true;  return; }

    // Clock line. AN3 going LOW just records the level; nothing shifts.
    if (addr == 0xC05E) { an3Prev = false; return; }

    // AN3 going HIGH: rising edge → push the current data bit into the
    // FIFO. We only shift on a real transition; a software that hammers
    // $C05F repeatedly without an intervening $C05E gets one shift, not
    // many — that matches the Arlequin reference sequence which alternates
    // STA $C05E ; STA $C05F for every bit.
    if (addr == 0xC05F) {
        if (!an3Prev) clockFifo(eightyColLatched);
        an3Prev = true;
        return;
    }
}

void LeChatMauveCard::clockFifo(bool dataBit)
{
    fifo = static_cast<uint8_t>(((fifo << 1) | (dataBit ? 1u : 0u)) & 0b11);
    mode = static_cast<RenderMode>(fifo);
}

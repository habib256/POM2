// POM2 Apple II Emulator
// Copyright (C) 2026

#include "LeChatMauveCard.h"

void LeChatMauveCard::onReset()
{
    // Real Féline cards default to COL140 at power-up — the FIFO bits
    // are pulled to 1 by the Péritel output stage. AN3/80COL latched
    // levels are cleared so a STA $C05E right after reset is correctly
    // recorded as "AN3 went low" (no spurious rising edge later).
    fifo             = 0b11;
    mode             = RenderMode::COL140;
    an3Prev          = false;
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

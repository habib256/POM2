// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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
        // We clock 80COL *directly* into the FIFO — MAME's convention. AppleWin
        // clocks the INVERSE (!80COL), so the same software ends up selecting
        // the bit-inverse RenderMode there. POM2 follows MAME, so the mode
        // numbering here is the bit-inverse of AppleWin's when tracing the same
        // STA $C05E/$C05F bit sequence — expected, not a bug.
        if (!an3Prev) clockFifo(eightyColLatched);
        an3Prev = true;
        return;
    }

    // Eve-class extension registers (no FIFO involvement — direct level
    // toggles). $C0B8/9 = Color TEXT master enable, $C0BA/B = HGR
    // Duochrome enable. Any access (read or write) flips the bit; that
    // matches the documented "bascule" semantics of the brevet and lets
    // a `LDA $C0B9` strobe enable the mode just like the SET/CLR pair on
    // the standard Apple II soft switches.
    if (addr == 0xC0B8) { colorTextEnabled_    = false; return; }
    if (addr == 0xC0B9) { colorTextEnabled_    = true;  return; }
    if (addr == 0xC0BA) { hgrDuochromeEnabled_ = false; return; }
    if (addr == 0xC0BB) { hgrDuochromeEnabled_ = true;  return; }
}

void LeChatMauveCard::clockFifo(bool dataBit)
{
    fifo = static_cast<uint8_t>(((fifo << 1) | (dataBit ? 1u : 0u)) & 0b11);
    mode = static_cast<RenderMode>(fifo);
}

void LeChatMauveCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    // v2: the two Eve extension toggles ride along. They were documented
    // as "user settings, not guest-volatile state" and left out — but the
    // $C0B8-$C0BB decode above mutates them from the GUEST bus, so a
    // rewind past a `STA $C0BB` left the display stuck in HGR Duochrome
    // with only the UI checkbox to recover it.
    out.push_back('C'); out.push_back('M'); out.push_back(2);
    out.push_back(fifo);
    out.push_back(static_cast<uint8_t>(mode));
    out.push_back(an3Prev ? 1 : 0);
    out.push_back(eightyColLatched ? 1 : 0);
    out.push_back(colorTextEnabled_    ? 1 : 0);
    out.push_back(hgrDuochromeEnabled_ ? 1 : 0);
}

void LeChatMauveCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    // v1 blobs (7 bytes, no Eve toggles) still load — they simply leave
    // the live toggle state alone, the pre-fix behaviour.
    if (len < 7 || data[0] != 'C' || data[1] != 'M' ||
        (data[2] != 1 && data[2] != 2)) return;
    fifo             = static_cast<uint8_t>(data[3] & 0b11);
    mode             = static_cast<RenderMode>(data[4] & 0b11);
    an3Prev          = data[5] != 0;
    eightyColLatched = data[6] != 0;
    if (data[2] >= 2 && len >= 9) {
        colorTextEnabled_    = data[7] != 0;
        hgrDuochromeEnabled_ = data[8] != 0;
    }
}

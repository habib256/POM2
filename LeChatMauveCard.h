// POM2 Apple II Emulator
// Copyright (C) 2026
//
// LeChatMauveCard — Apple II/II+ RGB video adapter (Péritel/SCART output)
// emulating the French "Le Chat Mauve" family (RVB Graph, Eve, Féline) and
// its US-licensed cousin (Video-7 AppleColor RGB / Apple Color Card).
//
// On real hardware these cards sniff the digital video signal and synthesize
// a clean RGB output — no NTSC artifact decoding, no inter-byte fringing,
// two distinct grays for the $5 and $A bit patterns (NTSC averages them
// into a single neutral). They occupy an expansion slot but expose **no**
// memory-mapped I/O and **no** slot ROM — they are passive observers of
// two soft switches:
//
//   $C00C / $C00D   80COL — used here as the FIFO data line
//   $C05E / $C05F   AN3   — used here as the FIFO clock (rising edge)
//
// The card holds a 2-bit shift register. Each rising edge of AN3 (i.e. each
// $C05E→$C05F transition) pushes the current 80COL bit into the FIFO,
// selecting one of four render modes:
//
//     00  BW560      Strict monochrome (560-pixel-equivalent on DHGR;
//                    on standard HGR it just disables color decoding).
//     01  Mixed      DHGR mode: 140-color top + 560-mono bottom. On
//                    standard HGR we fall back to ChatMauve-RGB.
//     10  Chunky160  Video-7 chunky byte mode. On standard HGR we fall
//                    back to ChatMauve-RGB (DHGR not modelled).
//     11  COL140     Default at reset — RGB 16-color decoding.
//
// The card is purely a renderer-mode selector: Apple2Display queries
// `currentMode()` each frame to choose between the NTSC pipeline (no card)
// and the ChatMauve RGB / Mono pipelines (card present).
//
// Slot 7 by convention (Apple Color Card / Video-7 historical placement).
// Slot 0 is the language-card slot; slots 1-5 are typically printer/serial;
// slot 6 is taken by Disk II in this emulator.
//
// Hors scope (cf. TODO.md section 12):
//   - DHGR proper (560×192) — needs aux RAM + 80STORE/RAMRD/RAMWRT
//   - Eve Color TEXT mode ($C0B9) — needs aux RAM for per-cell attributes

#ifndef POM2_LE_CHAT_MAUVE_CARD_H
#define POM2_LE_CHAT_MAUVE_CARD_H

#include "SlotPeripheral.h"

#include <cstdint>
#include <string_view>

class LeChatMauveCard : public SlotPeripheral
{
public:
    enum class RenderMode : uint8_t {
        BW560     = 0b00,
        Mixed     = 0b01,
        Chunky160 = 0b10,
        COL140    = 0b11,
    };

    LeChatMauveCard() = default;

    std::string_view name() const override { return "Le Chat Mauve"; }

    // Apple II RESET — re-arm the FIFO to the Féline default (COL140).
    // The card ships in this state from the factory; the boot ROM
    // doesn't reconfigure it, so games that don't talk to AN3+80COL
    // get the RGB rendering "for free".
    void onReset() override;

    // System soft-switch broadcast from Memory::softSwitchAccess().
    // The only addresses we react to are $C00C/$C00D (data) and
    // $C05E/$C05F (clock); anything else is ignored.
    void onVideoSoftSwitch(uint16_t addr) override;

    // Read-only accessors used by Apple2Display and the UI panel.
    RenderMode currentMode() const { return mode; }
    uint8_t    fifoBits()    const { return fifo; }
    bool       eightyCol()   const { return eightyColLatched; }
    bool       an3High()     const { return an3Prev; }

    // UI override: force a render mode independent of the FIFO. Useful for
    // testing the four modes without a 6502 program driving them. Resets
    // back to the FIFO-driven mode on the next AN3 rising edge.
    void overrideMode(RenderMode m) { mode = m; fifo = static_cast<uint8_t>(m); }

private:
    bool        an3Prev          = false;          // last seen AN3 level
    bool        eightyColLatched = false;          // last seen 80COL level
    uint8_t     fifo             = 0b11;           // 2 bits, MSB shifted out
    RenderMode  mode             = RenderMode::COL140;

    void clockFifo(bool dataBit);
};

#endif // POM2_LE_CHAT_MAUVE_CARD_H

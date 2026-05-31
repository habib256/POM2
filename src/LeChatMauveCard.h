// VERHILLE Arnaud 2026

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
//     00  BW560      Strict monochrome 560-dot DHR (no chroma).
//     01  Mixed      Per-byte: MSB set → 4-bit color cell, MSB clear →
//                    7-dot bit-mapped mono (Video-7 "mixed" mode).
//     10  Chunky160  Video-7 160-wide chunky bytes: aux+(main<<8) → four
//                    4-bit pixels of three dots each, centred in 560.
//     11  COL140     Default at reset — RGB 16-color 140-cell decoding.
//
// The card is purely a renderer-mode selector: Apple2Display queries
// `currentMode()` each frame to choose between the NTSC pipeline (no card)
// and the ChatMauve RGB / Mono pipelines (card present). The full DHGR
// render of all four modes — plus the Video-7 foreground-background colored
// TEXT mode (40-col text + AN3, per-cell fg/bg nibbles in aux RAM) — is
// implemented in Apple2Display::renderDhgr / renderTextChatMauveFgBg, ported
// from MAME apple2video.cpp dhgr_update()/render_line_color_array() and
// pinned by tests/video7_parity_smoke_test.cpp.
//
// Slot 7 by convention (Apple Color Card / Video-7 historical placement).
// Slot 0 is the language-card slot; slots 1-5 are typically printer/serial;
// slot 6 is taken by Disk II in this emulator.

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

    static constexpr int kDefaultSlot = 7;

    /// The card has no slot ROM and no device-select space (it sniffs
    /// $C00C/$C00D and $C05E/$C05F via the video soft-switch broadcast),
    /// so the slot number is purely informational — held for the UI /
    /// diagnostics panel.
    explicit LeChatMauveCard(int slot = kDefaultSlot) : slot_(slot) {}

    int getSlot() const { return slot_; }

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

    // Dragon Wars compatibility: a handful of titles (the eponymous one
    // being the canonical case) encode their DHGR Mixed-mode data with
    // bit 7 inverted relative to the Video-7 / Le Chat Mauve spec — colour
    // cells where the brevet expects mono, and vice-versa. Setting this
    // toggle to true XORs the bit-7 (mode flag in DHGR Mixed, bank flag
    // in standard HGR) at decode time, restoring the intended rendering.
    // Matches AppleWin's `-rgb-card-invert-bit7` CLI switch. Off = strict
    // brevet semantics (default).
    void setInvertBit7(bool v) { invertBit7_ = v; }
    bool invertBit7() const    { return invertBit7_; }

    // Eve Color TEXT master enable — soft-switched on real hardware at
    // $C0B9 (set) / $C0B8 (clear). Default = enabled so software that
    // doesn't poke the Eve register still gets the Video-7 fg/bg text
    // path that has been live for a while (preserves backward
    // compatibility with the rest of POM2's IIe TEXT+AN3 pipeline).
    void setColorTextEnabled(bool v) { colorTextEnabled_ = v; }
    bool colorTextEnabled() const    { return colorTextEnabled_; }

    // Eve HGR Duochrome — soft-switched at $C0BB (set) / $C0BA (clear).
    // When on, standard HGR ($2000-$3FFF in MAIN) renders with per-byte
    // foreground/background colour pairs sourced from AUX at the matching
    // address (high nibble = fg lo-res index, low nibble = bg). Off
    // (default) keeps the 6-colour HGR bank/pair decode the Féline and
    // Video-7 ship with.
    void setHgrDuochromeEnabled(bool v) { hgrDuochromeEnabled_ = v; }
    bool hgrDuochromeEnabled() const    { return hgrDuochromeEnabled_; }

private:
    int         slot_;
    bool        an3Prev          = true;           // AN3 powers up HIGH (DHIRES off)
    bool        eightyColLatched = false;          // last seen 80COL level
    uint8_t     fifo             = 0b11;           // 2 bits, MSB shifted out
    RenderMode  mode             = RenderMode::COL140;
    bool        invertBit7_      = false;          // Dragon Wars compatibility
    bool        colorTextEnabled_   = true;        // Eve $C0B8/$C0B9
    bool        hgrDuochromeEnabled_= false;       // Eve $C0BA/$C0BB

    void clockFifo(bool dataBit);
};

#endif // POM2_LE_CHAT_MAUVE_CARD_H

// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MouseCardAppleWin — high-level emulation of the Apple II Mouse Interface
// card, ported from AppleWin's `source/MouseInterface.cpp` (class
// CMouseInterface). The MAME-faithful sibling `MouseCard` emulates the
// MC68705P3 microcontroller cycle-by-cycle from the Apple 341-0269 mask
// ROM; this variant follows AppleWin's approach instead: the MC6821 PIA
// is real (shared MC6821 chip model) but the MCU side is *synthesised* in
// C++ — reads/writes through the PIA are interpreted as a command byte
// stream and replied to with mouse state computed directly from host
// input. As a result this card only needs the slot EPROM
// (`mouse_341-0270-c.bin`); no 341-0269 MCU mask ROM required.
//
// Protocol mirrored from AppleWin's OnCommand / OnWrite:
//
//   $00 MOUSE_SET     1-byte    Set mode (firmware writes MODE_MOUSE_ON /
//                                MODE_INT_VBL / MODE_INT_BUTTON / etc.)
//   $10 MOUSE_READ    6-byte    Read X-lo, X-hi, Y-lo, Y-hi, status
//   $20 MOUSE_SERV    2-byte    Read pending IRQ source + clear CPU IRQ
//   $30 MOUSE_CLEAR   1-byte    Clear position + state
//   $40 MOUSE_POS     5-byte    Set absolute position
//   $50 MOUSE_INIT    3-byte    Init (clamp 0..1023, pos = 0)
//   $60 MOUSE_CLAMP   5-byte    Set X or Y clamp window (LSB of cmd byte
//                                = axis select: 0 = X, 1 = Y)
//   $70 MOUSE_HOME    1-byte    Re-home to (0, 0)
//   $90 MOUSE_TIME    1..4 byte VBL-time command (no-op in HLE)
//
// PIA Port B handshake (AppleWin's On6821_B): the firmware uses BIT5 of
// Port B as a write-strobe (1→0 = "byte on Port A is for the MCU") and
// BIT4 as a read-strobe (1→0 = "advance to next reply byte"). The
// command buffer fills up command-then-data; the first byte's high
// nibble selects the command and sets `nDataLen`. PIA Port B bits 1..3
// drive the slot-ROM bank (8 banks × 256 B), exactly like the real card.
// BIT6 and BIT7 are status bits driven *back* to the firmware (read-ack
// + write-ack) so the firmware's polling loops complete.
//
// Interrupt model: `OnMouseEvent` is called on every host input change
// and (optionally) once per emulated VBL — it sets the matching bits of
// `byState` (movement / button / VBL) under the current mode mask and
// raises the slot IRQ. The next MOUSE_SERV command clears it. AppleWin's
// `CpuIrqAssert(IS_MOUSE)` maps to `SlotPeripheral::assertIrq(true)`.

#ifndef POM2_MOUSE_CARD_APPLEWIN_H
#define POM2_MOUSE_CARD_APPLEWIN_H

#include "MC6821.h"
#include "SlotPeripheral.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

class MouseCardAppleWin : public SlotPeripheral
{
public:
    static constexpr int kDefaultSlot = 4;

    explicit MouseCardAppleWin(int slot = kDefaultSlot);

    int getSlot() const { return slot_; }

    /// Load the Apple 341-0270-c slot EPROM (2048 bytes). No second ROM
    /// needed — the MCU side is HLE'd. Returns false on size mismatch or
    /// open failure.
    bool loadRom(const std::string& slotRomPath);
    bool isReady() const { return slotRomLoaded; }

    /// Host-mouse position update — same signature as MouseCard so the UI
    /// layer in MainWindow can drive either variant. `rawX`/`rawY` are
    /// running 8-bit counters (the screen-hole closed-loop in MainWindow
    /// drives this). Internally we compute signed 8-bit deltas with wrap
    /// correction and apply them via the firmware's absolute clamp window.
    void setHostMouse(uint8_t rawX, uint8_t rawY, bool button);

    /// Snapshot of internal mouse-firmware state for the Mouse Inspector
    /// panel. Not part of the AppleWin protocol — POM2-only diagnostic
    /// view of the HLE'd MCU's working set: clamp window, current
    /// position iX/iY (firmware-resolved cursor inside the clamp),
    /// last-MOUSE_READ snapshot nX/nY, button shadows, mode/state bytes,
    /// and the PIA port latches. Read on the UI thread; the underlying
    /// fields are scalars touched by the CPU thread, so values may be
    /// momentarily stale but never torn for the purposes of a UI panel.
    struct DebugSnapshot {
        int iX, iY;
        int nX, nY;
        int iMinX, iMaxX;
        int iMinY, iMaxY;
        bool bBtn0, bBtn1;
        bool bPrevBtn0, bPrevBtn1;
        uint8_t byMode;
        uint8_t byState;
        uint8_t by6821A;
        uint8_t by6821B;
        int     buffPos;
        int     dataLen;
        uint8_t lastCmd;       // byBuff[0]
    };
    DebugSnapshot debugSnapshot() const;

    // ─── SlotPeripheral overrides ──────────────────────────────────────
    std::string_view name() const override { return "Mouse (AppleWin HLE)"; }
    uint8_t deviceSelectRead (uint8_t low4) override;
    void    deviceSelectWrite(uint8_t low4, uint8_t v) override;
    uint8_t slotRomRead(uint8_t low8) override;
    void    advanceCycles(int cycles) override;
    void    onReset() override;
    // //c-class punches the forced INTCXROM mask for this card's $Cn00
    // firmware so PR#4 runs the AppleWin EPROM (which drives our PIA at
    // $C0C0) instead of the //c's on-board mouse firmware (which would
    // poke IOU hardware POM2 doesn't model — making the mouse a no-op).
    bool    exposesIicOnboardRom() const override { return slotRomLoaded; }

private:
    int      slot_;
    MC6821   pia;
    std::array<uint8_t, 0x800> slotRom{};
    bool     slotRomLoaded = false;

    // ── PIA Port A/B latch shadows (AppleWin m_by6821A / m_by6821B). ──
    uint8_t  by6821A = 0;
    uint8_t  by6821B = 0x40;        // BIT6 starts set — matches AppleWin Reset

    // ── Command-byte buffer (AppleWin m_byBuff / m_nBuffPos / m_nDataLen).
    uint8_t  byBuff[8] = { 0 };
    int      nBuffPos  = 0;
    int      nDataLen  = 1;

    // ── HLE'd MCU state (AppleWin m_byState / m_byMode / m_iX/Y / clamps).
    uint8_t  byState   = 0;
    uint8_t  byMode    = 0;
    int      iX = 0, iY = 0;
    int      nX = 0, nY = 0;
    int      iMinX = 0, iMaxX = 1023;
    int      iMinY = 0, iMaxY = 1023;
    bool     bButtons[2] = { false, false };
    bool     bBtn0 = false, bBtn1 = false;

    // ── Host input shadow (UI → CPU thread). ──────────────────────────
    std::atomic<uint8_t> hostX     { 0 };
    std::atomic<uint8_t> hostY     { 0 };
    std::atomic<bool>    hostButton{ false };
    uint8_t  lastHostX = 0;
    uint8_t  lastHostY = 0;
    bool     lastHostButton = false;
    bool     hostPrimed = false;

    // ── VBL pacing (~17030 cycles/frame at 1 MHz). ───────────────────
    int      vblCycleAccum = 0;

    // ── Internal hooks (AppleWin parity) ─────────────────────────────
    void onPiaPortAOut(uint8_t v);    // = On6821_A
    void onPiaPortBOut(uint8_t v);    // = On6821_B
    void onCommand();
    void onWrite();
    void onMouseEvent(bool vbl);
    void clearState();
    int  clampX();
    int  clampY();
    void setClampX(int lo, int hi);
    void setClampY(int lo, int hi);
    void setPositionAbs(int x, int y);
    void setPositionRel(int dx, int dy);
    void setButton(int idx, bool down);
    void pollHostInput();             // pump atomics → setPositionRel/Button
};

#endif // POM2_MOUSE_CARD_APPLEWIN_H

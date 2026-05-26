// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MouseCardAppleWin — see header. Direct port of AppleWin's
// `source/MouseInterface.cpp` (CMouseInterface). The constants, command
// dispatch, bit-handshake on Port B, and OnMouseEvent gating are
// transcribed line-for-line so future AppleWin updates can be diffed
// against POM2 without re-deriving the protocol.

#include "MouseCardAppleWin.h"
#include "Logger.h"

#include <fstream>

namespace {

// AppleWin MouseInterface.cpp — command opcodes (high nibble of first
// byte written to the MCU command port).
constexpr uint8_t MOUSE_SET   = 0x00;
constexpr uint8_t MOUSE_READ  = 0x10;
constexpr uint8_t MOUSE_SERV  = 0x20;
constexpr uint8_t MOUSE_CLEAR = 0x30;
constexpr uint8_t MOUSE_POS   = 0x40;
constexpr uint8_t MOUSE_INIT  = 0x50;
constexpr uint8_t MOUSE_CLAMP = 0x60;
constexpr uint8_t MOUSE_HOME  = 0x70;
constexpr uint8_t MOUSE_TIME  = 0x90;

constexpr uint8_t BIT4 = 0x10;
constexpr uint8_t BIT5 = 0x20;
constexpr uint8_t BIT6 = 0x40;
constexpr uint8_t BIT7 = 0x80;

// Status bits returned to the firmware in MOUSE_READ / MOUSE_SERV.
constexpr uint8_t STAT_PREV_BUTTON1             = 1 << 0;
constexpr uint8_t STAT_INT_MOVEMENT             = 1 << 1;
constexpr uint8_t STAT_INT_BUTTON               = 1 << 2;
constexpr uint8_t STAT_INT_VBL                  = 1 << 3;
constexpr uint8_t STAT_CURR_BUTTON1             = 1 << 4;
constexpr uint8_t STAT_MOVEMENT_SINCE_READMOUSE = 1 << 5;
constexpr uint8_t STAT_PREV_BUTTON0             = 1 << 6;
constexpr uint8_t STAT_CURR_BUTTON0             = 1 << 7;
constexpr uint8_t STAT_INT_ALL =
    STAT_INT_VBL | STAT_INT_BUTTON | STAT_INT_MOVEMENT;

// MODE bits (MOUSE_SET argument, latched into byMode).
constexpr uint8_t MODE_MOUSE_ON     = 1 << 0;
constexpr uint8_t MODE_INT_MOVEMENT = 1 << 1;
constexpr uint8_t MODE_INT_BUTTON   = 1 << 2;
constexpr uint8_t MODE_INT_VBL      = 1 << 3;
constexpr uint8_t MODE_INT_ALL      = STAT_INT_ALL;

// 1.022727 MHz / 60 Hz ≈ 17045 cycles per VBL.
constexpr int kCyclesPerVbl = 17045;

}  // namespace

MouseCardAppleWin::MouseCardAppleWin(int slot)
    : slot_(slot)
{
    pia.setPortAWriteCallback([this](uint8_t v) { onPiaPortAOut(v); });
    pia.setPortBWriteCallback([this](uint8_t v) { onPiaPortBOut(v); });

    // AppleWin Reset(): m_by6821B = 0x40 → BIT6 (read-strobe ack) set.
    pia.setPortAInput(0);
    pia.setPortBInput(by6821B);
    pia.setCB1(true);
}

bool MouseCardAppleWin::loadRom(const std::string& slotRomPath)
{
    std::ifstream f(slotRomPath, std::ios::binary);
    if (!f) {
        pom2::log().warn("MouseAW", "Cannot open slot ROM: " + slotRomPath);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size != 0x800) {
        pom2::log().warn("MouseAW",
            "Slot ROM size mismatch (" + std::to_string(size) +
            " bytes, expected 2048): " + slotRomPath);
        return false;
    }
    f.read(reinterpret_cast<char*>(slotRom.data()), 0x800);
    if (!f) {
        pom2::log().warn("MouseAW", "Short read on slot ROM: " + slotRomPath);
        return false;
    }
    slotRomLoaded = true;
    pom2::log().info("MouseAW",
        "Loaded slot ROM " + slotRomPath + " (2048 bytes)");
    return true;
}

void MouseCardAppleWin::setHostMouse(uint8_t rawX, uint8_t rawY, bool button)
{
    hostX.store(rawX, std::memory_order_relaxed);
    hostY.store(rawY, std::memory_order_relaxed);
    hostButton.store(button, std::memory_order_relaxed);
}

MouseCardAppleWin::DebugSnapshot MouseCardAppleWin::debugSnapshot() const
{
    DebugSnapshot s{};
    s.iX        = iX;        s.iY        = iY;
    s.nX        = nX;        s.nY        = nY;
    s.iMinX     = iMinX;     s.iMaxX     = iMaxX;
    s.iMinY     = iMinY;     s.iMaxY     = iMaxY;
    s.bBtn0     = bButtons[0];
    s.bBtn1     = bButtons[1];
    s.bPrevBtn0 = bBtn0;
    s.bPrevBtn1 = bBtn1;
    s.byMode    = byMode;
    s.byState   = byState;
    s.by6821A   = by6821A;
    s.by6821B   = by6821B;
    s.buffPos   = nBuffPos;
    s.dataLen   = nDataLen;
    s.lastCmd   = byBuff[0];
    return s;
}

// ─── SlotPeripheral ───────────────────────────────────────────────────────

uint8_t MouseCardAppleWin::deviceSelectRead(uint8_t low4)
{
    return pia.read(static_cast<uint8_t>(low4 & 0x03));
}

void MouseCardAppleWin::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    pia.write(static_cast<uint8_t>(low4 & 0x03), v);
}

uint8_t MouseCardAppleWin::slotRomRead(uint8_t low8)
{
    // AppleWin SetSlotRom: bank = (m_by6821B << 7) & 0x0700. POM2 reads
    // the bank on demand instead of memcpy'ing into peripheral ROM.
    const uint16_t bank = static_cast<uint16_t>((by6821B << 7) & 0x0700);
    return slotRom[static_cast<size_t>(low8) | bank];
}

void MouseCardAppleWin::advanceCycles(int cycles)
{
    if (!isReady() || cycles <= 0) return;

    // Drain any host motion / button accumulated since the last tick. This
    // lets the firmware see input as soon as it polls, without waiting for
    // the next VBL boundary.
    pollHostInput();

    vblCycleAccum += cycles;
    while (vblCycleAccum >= kCyclesPerVbl) {
        vblCycleAccum -= kCyclesPerVbl;
        onMouseEvent(/*vbl=*/true);
    }
}

void MouseCardAppleWin::onReset()
{
    // AppleWin Reset() — verbatim defaults.
    by6821A = 0;
    by6821B = 0x40;
    pia.reset();
    pia.setCB1(true);
    pia.setPortBInput(by6821B);

    byMode = 0;
    nX = nY = 0;
    iX = iY = 0;
    iMinX = 0; iMaxX = 1023;
    iMinY = 0; iMaxY = 1023;
    bButtons[0] = bButtons[1] = false;

    clearState();
    for (auto& b : byBuff) b = 0;

    lastHostX = lastHostY = 0;
    lastHostButton = false;
    hostPrimed = false;
    vblCycleAccum = 0;

    assertIrq(false);
}

// ─── PIA → MouseCard bridge (verbatim from AppleWin On6821_A/B) ───────────

void MouseCardAppleWin::onPiaPortAOut(uint8_t v)
{
    // On6821_A: just stash. The byte is consumed by OnCommand / OnWrite
    // when the firmware drops BIT5 of Port B (write-strobe).
    by6821A = v;
}

void MouseCardAppleWin::onPiaPortBOut(uint8_t v)
{
    // AppleWin On6821_B (verbatim): handshake on BIT5 (write-strobe) and
    // BIT4 (read-strobe); only react when at least one bit in 0x3E
    // changed (the firmware-driven nibble).
    const uint8_t byDiff = (by6821B ^ v) & 0x3E;
    if (!byDiff) return;

    by6821B = static_cast<uint8_t>((by6821B & ~0x3E) | (v & 0x3E));

    if (byDiff & BIT5) {
        if (v & BIT5) {
            // 0→1 on BIT5 — just signal write-ack. Pair with the 1→0
            // edge below that actually consumes the byte.
            by6821B |= BIT7;
        } else {
            // 1→0 — consume Port A as the next command/data byte.
            byBuff[nBuffPos++] = by6821A;
            if (nBuffPos == 1) onCommand();
            if (nBuffPos == nDataLen || nBuffPos > 7) {
                onWrite();
                nBuffPos = 0;
            }
            by6821B &= ~BIT7;
            pia.setPortBInput(by6821B);
        }
    }
    if (byDiff & BIT4) {
        if (v & BIT4) {
            by6821B &= ~BIT6;
        } else {
            // Read-strobe: deliver the next buffered reply byte to Port A.
            if (nBuffPos) ++nBuffPos;
            if (nBuffPos == nDataLen || nBuffPos > 7) {
                nBuffPos = 0;
            } else {
                pia.setPortAInput(byBuff[nBuffPos]);
            }
            by6821B |= BIT6;
        }
    }
    pia.setPortBInput(by6821B);
}

// ─── HLE'd MCU command dispatch (AppleWin OnCommand / OnWrite) ────────────

void MouseCardAppleWin::onCommand()
{
    switch (byBuff[0] & 0xF0) {
    case MOUSE_SET:
        nDataLen = 1;
        byMode = static_cast<uint8_t>(byBuff[0] & 0x0F);
        break;
    case MOUSE_READ:
        nDataLen = 6;
        byState &= STAT_MOVEMENT_SINCE_READMOUSE;
        nX = iX;
        nY = iY;
        if (bBtn0) byState |= STAT_PREV_BUTTON0;
        if (bBtn1) byState |= STAT_PREV_BUTTON1;
        bBtn0 = bButtons[0];
        bBtn1 = bButtons[1];
        if (bBtn0) byState |= STAT_CURR_BUTTON0;
        if (bBtn1) byState |= STAT_CURR_BUTTON1;
        byBuff[1] = static_cast<uint8_t>(nX & 0xFF);
        byBuff[2] = static_cast<uint8_t>((nX >> 8) & 0xFF);
        byBuff[3] = static_cast<uint8_t>(nY & 0xFF);
        byBuff[4] = static_cast<uint8_t>((nY >> 8) & 0xFF);
        byBuff[5] = byState;
        byState &= ~STAT_MOVEMENT_SINCE_READMOUSE;
        break;
    case MOUSE_SERV:
        nDataLen = 2;
        byBuff[1] = static_cast<uint8_t>(byState & ~STAT_MOVEMENT_SINCE_READMOUSE);
        assertIrq(false);     // AppleWin: CpuIrqDeassert(IS_MOUSE)
        break;
    case MOUSE_CLEAR:
        clearState();
        nDataLen = 1;
        break;
    case MOUSE_POS:
        nDataLen = 5;
        break;
    case MOUSE_INIT:
        nDataLen = 3;
        byBuff[1] = 0xFF;
        break;
    case MOUSE_CLAMP:
        nDataLen = 5;
        break;
    case MOUSE_HOME:
        nDataLen = 1;
        setPositionAbs(0, 0);
        break;
    case MOUSE_TIME:
        switch (byBuff[0] & 0x0C) {
        case 0x00: nDataLen = 1; break;
        case 0x04: nDataLen = 3; break;
        case 0x08: nDataLen = 2; break;
        case 0x0C: nDataLen = 4; break;
        }
        break;
    case 0xA0:
        nDataLen = 2;
        break;
    case 0xB0:
    case 0xC0:
        nDataLen = 1;
        break;
    default:
        nDataLen = 1;
        break;
    }
    pia.setPortAInput(byBuff[1]);
}

void MouseCardAppleWin::onWrite()
{
    int nMin, nMax;
    switch (byBuff[0] & 0xF0) {
    case MOUSE_CLAMP:
        nMin = (byBuff[3] << 8) | byBuff[1];
        nMax = (byBuff[4] << 8) | byBuff[2];
        if (byBuff[0] & 1) setClampY(nMin, nMax);
        else               setClampX(nMin, nMax);
        break;
    case MOUSE_POS:
        nX = (byBuff[2] << 8) | byBuff[1];
        nY = (byBuff[4] << 8) | byBuff[3];
        setPositionAbs(nX, nY);
        break;
    case MOUSE_INIT:
        nX = 0;
        nY = 0;
        setClampX(0, 1023);
        setClampY(0, 1023);
        setPositionAbs(0, 0);
        break;
    }
}

void MouseCardAppleWin::onMouseEvent(bool vbl)
{
    uint8_t st = 0;

    if ((byMode & MODE_INT_VBL) && vbl)
        st |= STAT_INT_VBL;

    if (byMode & MODE_MOUSE_ON) {
        if (nX != iX || nY != iY) {
            st     |= STAT_INT_MOVEMENT | STAT_MOVEMENT_SINCE_READMOUSE;
            byState |= STAT_MOVEMENT_SINCE_READMOUSE;
        }
        if (bBtn0 != bButtons[0] || bBtn1 != bButtons[1])
            st |= STAT_INT_BUTTON;
        st &= static_cast<uint8_t>((byMode & MODE_INT_ALL) |
                                   STAT_MOVEMENT_SINCE_READMOUSE);
    } else {
        st &= STAT_INT_VBL;
    }

    if (st & STAT_INT_ALL) {
        byState |= st;
        assertIrq(true);     // AppleWin: CpuIrqAssert(IS_MOUSE)
    }
}

void MouseCardAppleWin::clearState()
{
    nBuffPos = 0;
    nDataLen = 1;
    byState  = 0;
    nX = nY = 0;
    bBtn0 = bBtn1 = false;
    setPositionAbs(0, 0);
}

int MouseCardAppleWin::clampX()
{
    if (iX > iMaxX) { iX = iMaxX; return  1; }
    if (iX < iMinX) { iX = iMinX; return -1; }
    return 0;
}

int MouseCardAppleWin::clampY()
{
    if (iY > iMaxY) { iY = iMaxY; return  1; }
    if (iY < iMinY) { iY = iMinY; return -1; }
    return 0;
}

void MouseCardAppleWin::setClampX(int lo, int hi)
{
    // AppleWin SetClampX: swapped-range trick — when lo > hi, treat as a
    // wrapped window with effective max = (lo+hi)&0xFFFF.
    if (static_cast<unsigned>(lo) > 0xFFFF ||
        static_cast<unsigned>(hi) > 0xFFFF) return;
    if (lo > hi) { int nh = (lo + hi) & 0xFFFF; lo = 0; hi = nh; }
    iMinX = lo;
    iMaxX = hi;
    clampX();
}

void MouseCardAppleWin::setClampY(int lo, int hi)
{
    if (static_cast<unsigned>(lo) > 0xFFFF ||
        static_cast<unsigned>(hi) > 0xFFFF) return;
    if (lo > hi) { int nh = (lo + hi) & 0xFFFF; lo = 0; hi = nh; }
    iMinY = lo;
    iMaxY = hi;
    clampY();
}

void MouseCardAppleWin::setPositionAbs(int x, int y)
{
    iX = x;
    iY = y;
}

void MouseCardAppleWin::setPositionRel(int dx, int dy)
{
    iX += dx; clampX();
    iY += dy; clampY();
    onMouseEvent(/*vbl=*/false);
}

void MouseCardAppleWin::setButton(int idx, bool down)
{
    if (idx < 0 || idx > 1) return;
    bButtons[idx] = down;
    onMouseEvent(/*vbl=*/false);
}

void MouseCardAppleWin::pollHostInput()
{
    const uint8_t hx = hostX.load(std::memory_order_relaxed);
    const uint8_t hy = hostY.load(std::memory_order_relaxed);
    const bool    hb = hostButton.load(std::memory_order_relaxed);

    if (!hostPrimed) {
        lastHostX = hx;
        lastHostY = hy;
        lastHostButton = hb;
        hostPrimed = true;
        return;
    }

    // 8-bit signed delta with wrap correction — matches the running
    // counter convention used by MAME-faithful MouseCard's setHostMouse.
    int dx = static_cast<int>(hx) - static_cast<int>(lastHostX);
    int dy = static_cast<int>(hy) - static_cast<int>(lastHostY);
    if (dx >  0x80) dx -= 0x100;
    else if (dx < -0x80) dx += 0x100;
    if (dy >  0x80) dy -= 0x100;
    else if (dy < -0x80) dy += 0x100;
    lastHostX = hx;
    lastHostY = hy;

    if (dx || dy) setPositionRel(dx, dy);

    if (hb != lastHostButton) {
        lastHostButton = hb;
        setButton(0, hb);
    }
}

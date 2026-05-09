// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MouseCard — see header. Verbatim port of MAME `bus/a2bus/mouse.cpp`,
// re-expressed for POM2's slot bus + the M68705P3 / MC6821 emulators
// just landed in Phases 2 + 3. Read this side-by-side with MAME's
// `a2bus_mouse_device::pia_out_a/b`, `mcu_port_a/b/c_*`, and
// `update_axis<>`.

#include "MouseCard.h"
#include "Logger.h"

#include <cstring>
#include <fstream>

namespace {

// MCU PB pin assignments (per MAME mouse.cpp lines 320-323).
constexpr uint8_t MCU_PB_X1     = 0x02;     // X gate
constexpr uint8_t MCU_PB_X0     = 0x01;     // X direction (0=left, 1=right)
constexpr uint8_t MCU_PB_Y0     = 0x04;     // Y direction (0=up,   1=down)
constexpr uint8_t MCU_PB_Y1     = 0x08;     // Y gate
constexpr uint8_t MCU_PB_BUTTON = 0x80;     // active LOW (0 = pressed)

// Sub-cycle ratio: MCU clock = 2× Apple II clock.
constexpr int MCU_CLOCK_NUMERATOR   = 2;
constexpr int MCU_CLOCK_DENOMINATOR = 1;

}  // namespace

MouseCard::MouseCard(int slot)
    : slot_(slot)
{
    // Wire callbacks: PIA → MouseCard → MCU input pins.
    pia.setPortAWriteCallback([this](uint8_t v) { onPiaPortAOut(v); });
    pia.setPortBWriteCallback([this](uint8_t v) { onPiaPortBOut(v); });

    // MCU output writes propagate the other way: MCU → MouseCard → PIA
    // input pins (or, for PB6, the slot IRQ line).
    mcu.setPortWriteCallback([this](int port) { onMcuPortWrite(port); });

    // Port-read hook: synthesize Port B (button + quadrature) on every
    // MCU-side read.
    mcu.setPortReadCallback([this](int port) { return onMcuPortRead(port); });

    // Initial pin states. Real chip starts with all input pins high
    // (open-drain pull-ups on Port A; Port B drivers idle high).
    mcu.setPortInput(0, 0xFF);     // PIA → MCU Port A: pulled up
    mcu.setPortInput(2, 0x0F);     // PIA PB[7..4] → MCU PC[3..0]: pulled up

    // CB1 tied high to a 10 kΩ pull-up (MAME mouse.cpp line 250).
    pia.setCB1(true);
}

bool MouseCard::loadRoms(const std::string& slotRomPath,
                         const std::string& mcuRomPath)
{
    // ── Slot EPROM (341-0270-c.4b) ────────────────────────────────────
    {
        std::ifstream f(slotRomPath, std::ios::binary);
        if (!f) {
            pom2::log().warn("Mouse",
                "Cannot open slot ROM: " + slotRomPath);
            return false;
        }
        f.seekg(0, std::ios::end);
        const auto size = static_cast<size_t>(f.tellg());
        f.seekg(0, std::ios::beg);
        if (size != 0x800) {
            pom2::log().warn("Mouse",
                "Slot ROM size mismatch (" + std::to_string(size) +
                " bytes, expected 2048): " + slotRomPath);
            return false;
        }
        f.read(reinterpret_cast<char*>(slotRom.data()), 0x800);
        if (!f) {
            pom2::log().warn("Mouse", "Short read on slot ROM: " + slotRomPath);
            return false;
        }
        slotRomLoaded = true;
        pom2::log().info("Mouse",
            "Loaded slot ROM " + slotRomPath + " (2048 bytes)");
    }

    // ── MCU EPROM (341-0269.2b) ───────────────────────────────────────
    if (!mcu.loadRomFile(mcuRomPath)) {
        slotRomLoaded = false;     // refuse to plug if MCU ROM missing
        return false;
    }

    return true;
}

void MouseCard::setHostMouse(uint8_t rawX, uint8_t rawY, bool button)
{
    hostX.store(rawX, std::memory_order_relaxed);
    hostY.store(rawY, std::memory_order_relaxed);
    hostButton.store(button, std::memory_order_relaxed);
}

// ─── SlotPeripheral interface ───────────────────────────────────────────

uint8_t MouseCard::deviceSelectRead(uint8_t low4)
{
    // MAME `read_c0nx`: forward straight to the PIA, masking to the 2
    // register-select bits. The other 14 bits of the device-select
    // window mirror the PIA registers.
    return pia.read(static_cast<uint8_t>(low4 & 0x03));
}

void MouseCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    pia.write(static_cast<uint8_t>(low4 & 0x03), v);
}

uint8_t MouseCard::slotRomRead(uint8_t low8)
{
    // MAME `read_cnxx`: `m_rom[offset | m_rom_bank]`. The bank is
    // determined by the most recent PIA Port B write (PB1..PB3).
    return slotRom[static_cast<size_t>(low8) | romBank];
}

void MouseCard::advanceCycles(int cycles)
{
    if (!isReady() || cycles <= 0) return;

    // MCU runs at 2× the Apple II clock — accumulate fractional cycles
    // to keep the ratio exact across calls.
    mcuCycleAccum += cycles * MCU_CLOCK_NUMERATOR;
    const int mcuCycles = mcuCycleAccum / MCU_CLOCK_DENOMINATOR;
    mcuCycleAccum -= mcuCycles * MCU_CLOCK_DENOMINATOR;
    if (mcuCycles > 0) (void)mcu.run(mcuCycles);
}

void MouseCard::onReset()
{
    mcu.reset();
    pia.reset();
    pia.setCB1(true);
    romBank = 0;
    portAtoMcu = 0xFF;
    portCtoMcu = 0x0F;
    lastAxis[0] = lastAxis[1] = 0;
    countAxis[0] = countAxis[1] = 0;
    portBState = 0xFF;
    raiseIrq(false);
    mcuCycleAccum = 0;
}

// ─── PIA → MCU bridge ───────────────────────────────────────────────────

void MouseCard::onPiaPortAOut(uint8_t v)
{
    // PIA Port A output → MCU Port A input. MAME `pia_out_a` ->
    // `set_port_a_in` (deferred via scheduler in MAME, immediate here).
    portAtoMcu = v;
    mcu.setPortInput(0, v);
}

void MouseCard::onPiaPortBOut(uint8_t v)
{
    // PIA Port B output:
    //   PB1..PB3  → EPROM A8..A10 (8-bank slot ROM bank-select)
    //   PB4..PB7  → MCU PC0..PC3 (high nibble shifted down)
    //   PB0       → "sync latch" (not modelled)
    romBank = static_cast<uint16_t>(v & 0x0E) << 7;

    portCtoMcu = static_cast<uint8_t>((v >> 4) & 0x0F);
    mcu.setPortInput(2, portCtoMcu);
}

// ─── MCU → PIA bridge ───────────────────────────────────────────────────

void MouseCard::onMcuPortWrite(int port)
{
    if (port == 0) {
        // MCU Port A output → PIA Port A input. MAME `mcu_port_a_w` ->
        // `m_pia->set_a_input(...)`.
        const uint8_t pins = mcu.getPortPins(0);
        pia.setPortAInput(pins);
        return;
    }
    if (port == 1) {
        // MCU Port B output: bit 6 = slot IRQ (active low).
        const uint8_t pins = mcu.getPortPins(1);
        const bool assert = !(pins & 0x40);
        raiseIrq(assert);
        return;
    }
    if (port == 2) {
        // MCU Port C output → PIA Port B input bits 4..7. MAME
        // `mcu_port_c_w` -> `m_pia->portb_w(data << 4)`.
        const uint8_t pins = mcu.getPortPins(2);
        pia.setPortBInput(static_cast<uint8_t>(pins << 4));
        return;
    }
}

// ─── MCU port-read synthesis (Port B = button + quadrature) ─────────────

uint8_t MouseCard::onMcuPortRead(int port)
{
    if (port == 1) return mcuPortBRead();
    if (port == 0) return portAtoMcu;
    if (port == 2) return portCtoMcu;
    return 0xFF;
}

uint8_t MouseCard::mcuPortBRead()
{
    // Mirrors MAME `mcu_port_b_r()` (mouse.cpp lines 316-340). On every
    // call: refresh button bit; emit at most one quadrature edge per
    // axis if there's pending motion.
    const bool buttonPressed = hostButton.load(std::memory_order_relaxed);
    if (buttonPressed) portBState &= ~MCU_PB_BUTTON;
    else               portBState |=  MCU_PB_BUTTON;

    updateAxis(0, MCU_PB_X0, MCU_PB_X1);     // X axis: dir=X0, clk=X1
    updateAxis(1, MCU_PB_Y0, MCU_PB_Y1);     // Y axis: dir=Y0, clk=Y1

    return portBState;
}

void MouseCard::updateAxis(int axis, uint8_t dirBit, uint8_t clkBit)
{
    // Read the current host position for this axis as an unsigned 8-bit
    // running counter. Compute delta from last seen value with 8-bit
    // wrap correction (matches MAME's diff-wrap handling).
    const uint8_t cur = (axis == 0)
        ? hostX.load(std::memory_order_relaxed)
        : hostY.load(std::memory_order_relaxed);
    int diff = static_cast<int>(cur) - static_cast<int>(lastAxis[axis]);
    if (diff > 0x80)       diff -= 0x100;
    else if (diff < -0x80) diff += 0x100;

    countAxis[axis] += diff;
    lastAxis[axis]   = cur;

    if (countAxis[axis] != 0) {
        portBState ^= clkBit;     // toggle CLK
        if (countAxis[axis] < 0) {
            ++countAxis[axis];
            if (portBState & clkBit) portBState &= ~dirBit;
        } else {
            --countAxis[axis];
            if (portBState & clkBit) portBState |=  dirBit;
        }
    }
}

// ─── IRQ line ───────────────────────────────────────────────────────────

void MouseCard::raiseIrq(bool assert)
{
    if (assert == slotIrqAsserted) return;
    slotIrqAsserted = assert;
    if (cpu_) cpu_->setIRQ(assert ? 1 : 0);
}

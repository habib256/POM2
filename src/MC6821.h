// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MC6821 — Motorola Peripheral Interface Adapter. Verbatim port of
// MAME's `src/devices/machine/6821pia.cpp` (lines 1-1119), stripped of
// the device_t / devcb framework and condensed to a self-contained class
// for use by the Apple II Mouse Card (MAME `bus/a2bus/mouse.cpp` instances
// a 6821 on the card to bridge between the host 6502 and the on-card
// MC68705P3 microcontroller).
//
// Register layout (4 registers, addressed by 2-bit offset):
//
//   $00  Port A data / DDR A   (selected by CRA bit 2)
//   $01  CRA — Control Register A
//   $02  Port B data / DDR B   (selected by CRB bit 2)
//   $03  CRB — Control Register B
//
// Control register bit map (matches MAME `pia6821_device` helpers):
//
//   bit 0  CA1/CB1 IRQ enable
//   bit 1  CA1/CB1 active edge select (1 = low→high, 0 = high→low)
//   bit 2  Output register select (0 = DDR access at offset 0/2,
//                                  1 = data port access)
//   bit 3  CA2/CB2 control bit (meaning depends on bit 4/5):
//          - in input mode (bit 5 = 0): bit 3 = IRQ enable
//          - in output mode bit 4 = 1 (set/reset): bit 3 = output value
//          - in output mode bit 4 = 0 (strobe):    bit 3 = strobe-E reset
//   bit 4  CA2/CB2 input edge or output mode select
//   bit 5  CA2/CB2 direction (0 = input, 1 = output)
//   bit 6  IRQ2 status (read-only)
//   bit 7  IRQ1 status (read-only)
//
// Side effects pinned by tests/mc6821_smoke_test.cpp:
//
//   * Reading port A clears IRQ-A1 and IRQ-A2; reading port B clears
//     IRQ-B1 and IRQ-B2. Reading the control register does NOT clear.
//   * Writing port A or DDR A re-fires the port-A output callback.
//   * Edge on CA1 only triggers IRQ if CRA bit 1 matches the new level
//     (low→high or high→low) and CRA bit 0 is set.
//   * Bits 6/7 of CRA/CRB are read-only and reflect IRQ flags, ignoring
//     writes (write masks the byte to 0x3F).
//   * IRQ output line is the OR of the two CR bits when the matching
//     enable bit is set: `(IRQ1 && CR0) || (IRQ2 && CR3)` for both A
//     and B sides.

#ifndef POM2_MC6821_H
#define POM2_MC6821_H

#include <cstdint>
#include <functional>

class MC6821
{
public:
    MC6821();

    void reset();

    // ─── 6502-side register access ──────────────────────────────────────
    /// `offset` is bits 0..1 of the bus address (RS0/RS1 lines).
    uint8_t read(uint8_t offset);
    void    write(uint8_t offset, uint8_t data);

    // ─── External pin drivers (called by the host wiring) ───────────────
    /// Drive a level onto a port pin from outside the chip. The PIA
    /// composes the read value as `(latch & DDR) | (input & ~DDR)`.
    void setPortAInput(uint8_t v);
    void setPortBInput(uint8_t v);
    /// Drive CA1 / CA2 / CB1 / CB2 input lines. true = high.
    void setCA1(bool level);
    void setCA2(bool level);
    void setCB1(bool level);
    void setCB2(bool level);

    // ─── Output side ────────────────────────────────────────────────────
    /// Composite output value of port A (latch where DDR=1, input pulled
    /// up where DDR=0). What an external observer would see on the pins.
    uint8_t getPortAOutput() const;
    uint8_t getPortBOutput() const;
    /// Current driven CA2 / CB2 levels (only meaningful when configured
    /// as outputs — otherwise these float per MAME's `*_output_z`).
    bool    getCA2Output() const { return out_ca2; }
    bool    getCB2Output() const { return out_cb2; }
    /// Active-low IRQ output lines. true = asserted.
    bool    irqA() const { return irq_a_state; }
    bool    irqB() const { return irq_b_state; }

    // ─── Output-change callbacks ────────────────────────────────────────
    using PortWriteFn = std::function<void(uint8_t)>;
    using LineWriteFn = std::function<void(bool)>;
    void setPortAWriteCallback(PortWriteFn fn) { out_a_cb = std::move(fn); }
    void setPortBWriteCallback(PortWriteFn fn) { out_b_cb = std::move(fn); }
    void setCA2WriteCallback  (LineWriteFn fn) { out_ca2_cb = std::move(fn); }
    void setCB2WriteCallback  (LineWriteFn fn) { out_cb2_cb = std::move(fn); }
    void setIrqACallback      (LineWriteFn fn) { irq_a_cb = std::move(fn); }
    void setIrqBCallback      (LineWriteFn fn) { irq_b_cb = std::move(fn); }

    // ─── Diagnostics ────────────────────────────────────────────────────
    uint8_t getDdrA() const { return ddr_a; }
    uint8_t getDdrB() const { return ddr_b; }
    uint8_t getCRA()  const { return ctl_a; }
    uint8_t getCRB()  const { return ctl_b; }
    uint8_t getOutA() const { return out_a; }
    uint8_t getOutB() const { return out_b; }

private:
    // Per-side state (suffixed _a / _b to mirror MAME's naming).
    uint8_t in_a   = 0xFF;     // input from external pins
    uint8_t in_b   = 0x00;
    bool    in_ca1 = true;     // input line levels
    bool    in_ca2 = true;
    bool    in_cb1 = false;
    bool    in_cb2 = false;
    uint8_t out_a  = 0;        // output latch
    uint8_t out_b  = 0;
    bool    out_ca2 = false;   // CA2/CB2 driven levels (if outputs)
    bool    out_cb2 = false;
    uint8_t ddr_a  = 0;
    uint8_t ddr_b  = 0;
    uint8_t ctl_a  = 0;        // CRA
    uint8_t ctl_b  = 0;        // CRB

    // IRQ flags. _irq_X1 = CA1/CB1 edge latch, _irq_X2 = CA2/CB2 edge
    // latch. Output line is the OR of the enabled-and-set flags.
    bool    irq_a1 = false, irq_a2 = false;
    bool    irq_b1 = false, irq_b2 = false;
    bool    irq_a_state = false;
    bool    irq_b_state = false;

    // Callbacks.
    PortWriteFn out_a_cb, out_b_cb;
    LineWriteFn out_ca2_cb, out_cb2_cb;
    LineWriteFn irq_a_cb, irq_b_cb;

    // ─── Helpers (mirror MAME static inlines) ───────────────────────────
    static bool irq1_enabled(uint8_t c)    { return  (c & 0x01) != 0; }
    static bool c1_low_to_high(uint8_t c)  { return  (c & 0x02) != 0; }
    static bool c1_high_to_low(uint8_t c)  { return  (c & 0x02) == 0; }
    static bool output_selected(uint8_t c) { return  (c & 0x04) != 0; }
    static bool irq2_enabled(uint8_t c)    { return  (c & 0x08) != 0; }
    static bool c2_set(uint8_t c)          { return  (c & 0x08) != 0; }
    static bool c2_low_to_high(uint8_t c)  { return  (c & 0x10) != 0; }
    static bool c2_high_to_low(uint8_t c)  { return  (c & 0x10) == 0; }
    static bool c2_set_mode(uint8_t c)     { return  (c & 0x10) != 0; }
    static bool c2_output(uint8_t c)       { return  (c & 0x20) != 0; }
    static bool c2_input(uint8_t c)        { return  (c & 0x20) == 0; }

    void updateInterrupts();
    void setOutCa2(bool level);
    void setOutCb2(bool level);
    void sendOutA();
    void sendOutB();

    uint8_t getInAValue() const;
    uint8_t getInBValue() const;
    uint8_t getOutAValue() const;
    uint8_t getOutBValue() const;
};

#endif // POM2_MC6821_H

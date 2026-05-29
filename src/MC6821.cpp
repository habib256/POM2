// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MC6821 — see header. Verbatim port of MAME's `6821pia.cpp`. Read
// inline against the MAME source line-by-line: the C++ structure is
// identical (constructor → reset → register read/write → CA/CB line
// inputs), only the device_t / devcb plumbing has been replaced with
// std::function callbacks.

#include "MC6821.h"

namespace {
constexpr uint8_t PIA_IRQ1 = 0x80;
constexpr uint8_t PIA_IRQ2 = 0x40;
}  // namespace

MC6821::MC6821()
{
    reset();
    in_a = 0xFF;
    in_b = 0;
}

void MC6821::reset()
{
    // MAME device_reset(): port A defaults to internal pull-ups (out_a
    // pushed as 0xFF below), port B starts three-state (we just leave
    // it 0 — Mouse Card firmware always programs DDR before reading).
    out_a = 0;
    out_ca2 = false;
    ddr_a = 0;
    ctl_a = 0;
    irq_a1 = false;
    irq_a2 = false;
    irq_a_state = false;
    out_b = 0;
    out_cb2 = false;
    ddr_b = 0;
    ctl_b = 0;
    irq_b1 = false;
    irq_b2 = false;
    irq_b_state = false;

    if (irq_a_cb)   irq_a_cb(false);
    if (irq_b_cb)   irq_b_cb(false);
    if (out_a_cb)   out_a_cb(0xFF);     // internal pull-ups
    if (out_ca2_cb) out_ca2_cb(true);
}

// ─── Interrupt aggregation (MAME update_interrupts) ─────────────────────

void MC6821::updateInterrupts()
{
    bool new_a = (irq_a1 && irq1_enabled(ctl_a)) || (irq_a2 && irq2_enabled(ctl_a));
    if (new_a != irq_a_state) {
        irq_a_state = new_a;
        if (irq_a_cb) irq_a_cb(new_a);
    }
    bool new_b = (irq_b1 && irq1_enabled(ctl_b)) || (irq_b2 && irq2_enabled(ctl_b));
    if (new_b != irq_b_state) {
        irq_b_state = new_b;
        if (irq_b_cb) irq_b_cb(new_b);
    }
}

// ─── Port read helpers (MAME get_in_a_value / get_in_b_value) ───────────

uint8_t MC6821::getInAValue() const
{
    // For Port A, when in output mode, external devices can drive pins
    // too. We omit MAME's `m_a_input_overrides_output_mask` because the
    // mouse card doesn't use it.
    return static_cast<uint8_t>((~ddr_a & in_a) | (ddr_a & out_a));
}

uint8_t MC6821::getInBValue() const
{
    if (ddr_b == 0xFF) return out_b;     // all output
    return static_cast<uint8_t>((out_b & ddr_b) | (in_b & ~ddr_b));
}

uint8_t MC6821::getOutAValue() const
{
    if (ddr_a == 0xFF) return out_a;
    return static_cast<uint8_t>((out_a & ddr_a) | (getInAValue() & ~ddr_a));
}

uint8_t MC6821::getOutBValue() const
{
    return static_cast<uint8_t>(out_b & ddr_b);
}

// ─── Bus reads (MAME read / port_X_r / control_X_r) ─────────────────────

uint8_t MC6821::read(uint8_t offset)
{
    switch (offset & 0x03) {
    case 0x00: {
        if (output_selected(ctl_a)) {
            uint8_t ret = getInAValue();
            // IRQ flags implicitly cleared by reading port A.
            irq_a1 = false;
            irq_a2 = false;
            updateInterrupts();
            // CA2 read-strobe: when CA2 is an output in pulse/strobe mode,
            // reading port A pulses it low (and back high if strobe-E-reset
            // is selected). Mirrors the CB2 write-strobe in write() case 0x02
            // and MAME pia6821_device::port_a_r.
            if (!c2_set_mode(ctl_a) && c2_output(ctl_a)) {
                setOutCa2(false);
                if (c2_set(ctl_a)) setOutCa2(true);
            }
            return ret;
        }
        return ddr_a;
    }
    case 0x01: {
        uint8_t ret = ctl_a;
        if (irq_a1) ret |= PIA_IRQ1;
        if (irq_a2 && c2_input(ctl_a)) ret |= PIA_IRQ2;
        return ret;
    }
    case 0x02: {
        if (output_selected(ctl_b)) {
            uint8_t ret = getInBValue();
            irq_b1 = false;
            irq_b2 = false;
            updateInterrupts();
            return ret;
        }
        return ddr_b;
    }
    case 0x03: {
        uint8_t ret = ctl_b;
        if (irq_b1) ret |= PIA_IRQ1;
        if (irq_b2 && c2_input(ctl_b)) ret |= PIA_IRQ2;
        return ret;
    }
    }
    return 0;
}

// ─── Bus writes (MAME write / port_X_w / control_X_w) ───────────────────

void MC6821::write(uint8_t offset, uint8_t data)
{
    switch (offset & 0x03) {
    case 0x00:
        if (output_selected(ctl_a)) {
            // port A write
            out_a = data;
            sendOutA();
        } else {
            // DDR A write
            if (ddr_a != data) {
                ddr_a = data;
                sendOutA();
            }
        }
        break;
    case 0x01: {
        // CRA write — bits 6/7 are read-only.
        data &= 0x3F;
        bool ca2_was_output = c2_output(ctl_a);
        ctl_a = data;
        if (c2_output(ctl_a)) {
            if (c2_set_mode(ctl_a)) {
                bool set = c2_set(ctl_a);
                if (!ca2_was_output || out_ca2 != set) setOutCa2(set);
            } else {
                if (!ca2_was_output || !out_ca2) setOutCa2(true);
            }
        } else if (ca2_was_output) {
            // CA2 reverted to input — pulled high.
            if (out_ca2_cb) out_ca2_cb(true);
        }
        updateInterrupts();
        break;
    }
    case 0x02:
        if (output_selected(ctl_b)) {
            out_b = data;
            sendOutB();
            // CB2 in write strobe mode: pulse low on every port-B write.
            if (!c2_set_mode(ctl_b) && c2_output(ctl_b)) {
                setOutCb2(false);
                // Strobe-E reset: bit 3 of CRB selects whether the strobe
                // self-clears at end of cycle.
                if (c2_set(ctl_b)) setOutCb2(true);
            }
        } else {
            if (ddr_b != data) {
                ddr_b = data;
                sendOutB();
            }
        }
        break;
    case 0x03: {
        // CRB write.
        data &= 0x3F;
        ctl_b = data;
        if (c2_output(ctl_b)) {
            bool temp = c2_set_mode(ctl_b) ? c2_set(ctl_b) : true;
            setOutCb2(temp);
        }
        updateInterrupts();
        break;
    }
    }
}

// ─── Output dispatch ────────────────────────────────────────────────────

void MC6821::sendOutA()
{
    if (out_a_cb) out_a_cb(getOutAValue());
}
void MC6821::sendOutB()
{
    if (out_b_cb) out_b_cb(getOutBValue());
}

void MC6821::setOutCa2(bool level)
{
    out_ca2 = level;
    if (out_ca2_cb) out_ca2_cb(level);
}
void MC6821::setOutCb2(bool level)
{
    if (level == out_cb2) return;
    out_cb2 = level;
    if (out_cb2_cb) out_cb2_cb(level);
}

// ─── External pin drivers ───────────────────────────────────────────────

void MC6821::setPortAInput(uint8_t v)
{
    in_a = v;
}

void MC6821::setPortBInput(uint8_t v)
{
    in_b = v;
}

void MC6821::setCA1(bool state)
{
    if ((in_ca1 != state) &&
        ((state && c1_low_to_high(ctl_a)) ||
         (!state && c1_high_to_low(ctl_a))))
    {
        irq_a1 = true;
        updateInterrupts();
        // CA2 in read-strobe mode with C1 reset: pulse high on CA1 edge.
        if (c2_output(ctl_a) && !c2_set_mode(ctl_a) && !c2_set(ctl_a) && !out_ca2) {
            setOutCa2(true);
        }
    }
    in_ca1 = state;
}

void MC6821::setCA2(bool state)
{
    if (c2_input(ctl_a) && (in_ca2 != state) &&
        ((state && c2_low_to_high(ctl_a)) ||
         (!state && c2_high_to_low(ctl_a))))
    {
        irq_a2 = true;
        updateInterrupts();
    }
    in_ca2 = state;
}

void MC6821::setCB1(bool state)
{
    if ((in_cb1 != state) &&
        ((state && c1_low_to_high(ctl_b)) ||
         (!state && c1_high_to_low(ctl_b))))
    {
        irq_b1 = true;
        updateInterrupts();
    }
    in_cb1 = state;
}

void MC6821::setCB2(bool state)
{
    if (c2_input(ctl_b) && (in_cb2 != state) &&
        ((state && c2_low_to_high(ctl_b)) ||
         (!state && c2_high_to_low(ctl_b))))
    {
        irq_b2 = true;
        updateInterrupts();
    }
    in_cb2 = state;
}

uint8_t MC6821::getPortAOutput() const { return getOutAValue(); }
uint8_t MC6821::getPortBOutput() const { return getOutBValue(); }

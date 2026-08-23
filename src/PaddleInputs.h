// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PaddleInputs — the Apple II game-port state, split out of the Memory
// god-object (TODO P2, "one concern per file"). Four paddles ($C064-$C067),
// three push-buttons ($C061-$C063), and the Open/Solid-Apple + Shift modifier
// keys OR'd onto PB0/PB1/PB2. Memory owns one of these and forwards its
// $C061-$C067 reads and $C070 re-arm to it; the only piece Memory keeps is
// the master `cycleCounter`, which the RC-discharge read needs and which is
// passed in rather than duplicated here.
//
// Threading: the modifier keys are written from the GLFW key callback (the UI
// thread) and read by the CPU worker, so they are atomics — exactly as they
// were as bare Memory members. The paddle value/button arrays are written
// only under stateMutex (MainWindow's joystick block takes lockState()) and
// read by the worker while it holds the same lock, so they need no atomics;
// `input_io_smoke` and `ui_worker_contention` pin both facts.

#ifndef POM2_PADDLE_INPUTS_H
#define POM2_PADDLE_INPUTS_H

#include <array>
#include <atomic>
#include <cstdint>

namespace pom2 {

class PaddleInputs
{
public:
    // ── Writers ───────────────────────────────────────────────────────────
    void setPaddle(int idx, uint8_t value) {
        if (idx >= 0 && idx < 4) value_[idx] = value;
    }
    void setPaddleButton(int idx, bool down) {
        if (idx >= 0 && idx < 3) button_[idx] = down;
    }
    void setOpenAppleKey (bool down) { openApple_.store(down,  std::memory_order_relaxed); }
    void setSolidAppleKey(bool down) { solidApple_.store(down, std::memory_order_relaxed); }
    void setShiftKey     (bool down) { shift_.store(down,      std::memory_order_relaxed); }

    // ── Push-button reads ($C061-$C063 bit 7) ─────────────────────────────
    // PB0/PB1 OR the paddle button with Open/Solid-Apple; PB2 also folds in
    // the //e Shift-key mod (SHK jumper), but only in IIe mode.
    bool button0() const {
        return button_[0] || openApple_.load(std::memory_order_relaxed);
    }
    bool button1() const {
        return button_[1] || solidApple_.load(std::memory_order_relaxed);
    }
    bool button2(bool iieMode) const {
        return button_[2] || (iieMode && shift_.load(std::memory_order_relaxed));
    }

    // ── Paddle read ($C064-$C067 bit 7) ───────────────────────────────────
    // The RC network is still charging — bit 7 high — while fewer than
    // `value * 11` cycles have elapsed since the last $C070 strobe.
    bool discharging(int idx, uint64_t cycleCounter) const {
        const uint64_t elapsed   = cycleCounter - latchCycle_;
        const uint64_t threshold = static_cast<uint64_t>(value_[idx]) * 11;
        return elapsed < threshold;
    }

    // $C070 (mirrored $C070-$C07F): re-arm the discharge timer.
    void rearm(uint64_t cycleCounter) { latchCycle_ = cycleCounter; }

    // ── Snapshot (only the latch cycle is serialised, as before) ──────────
    uint64_t latchCycle() const         { return latchCycle_; }
    void     setLatchCycle(uint64_t c)  { latchCycle_ = c; }

private:
    std::array<uint8_t, 4> value_ { 128, 128, 128, 128 };
    std::array<bool,    3> button_{ false, false, false };
    std::atomic<bool> openApple_ { false };
    std::atomic<bool> solidApple_{ false };
    std::atomic<bool> shift_     { false };
    // Boot-time value: cycleCounter - latchCycle_ must exceed every paddle
    // threshold (max 255*11 = 2805) at power-on so an un-strobed $C064-$C067
    // reads low — else the //e "no paddle connected" self-test is confused.
    // `0 - (~0 - 0xFFFF)` = 0x10000 > 2805. (Copied verbatim from the old
    // Memory member's rationale.)
    uint64_t latchCycle_ = ~uint64_t(0) - 0xFFFFu;
};

} // namespace pom2

#endif // POM2_PADDLE_INPUTS_H

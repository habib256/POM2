// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Apple II game-port buttons, IIe modifier wiring and 558-style paddle RC
// timers. Memory owns address decoding; this class owns input/timer state.

#ifndef POM2_PADDLE_INPUTS_H
#define POM2_PADDLE_INPUTS_H

#include <array>
#include <atomic>
#include <cstdint>

namespace pom2 {

class PaddleInputs
{
public:
    PaddleInputs();

    void setPaddle(int index, uint8_t value);
    void setButton(int index, bool down);
    void setOpenApple(bool down) noexcept { openApple_.store(down); }
    void setSolidApple(bool down) noexcept { solidApple_.store(down); }
    void setShift(bool down) noexcept { shift_.store(down); }

    /// Read bit 7 for canonical switch $61..$67 (not its mirrored address).
    uint8_t read(uint8_t canonicalLow, uint64_t cycle, bool iieMode) const;

    void strobe(uint64_t cycle) noexcept { latchCycle_ = cycle; }
    uint64_t latchCycle() const noexcept { return latchCycle_; }
    void restoreLatchCycle(uint64_t cycle) noexcept { latchCycle_ = cycle; }

private:
    std::array<std::atomic<uint8_t>, 4> values_;
    std::array<std::atomic<bool>, 3> buttons_;
    std::atomic<bool> openApple_{false};
    std::atomic<bool> solidApple_{false};
    std::atomic<bool> shift_{false};
    uint64_t latchCycle_ = ~uint64_t(0) - 0xFFFFu;
};

} // namespace pom2

#endif // POM2_PADDLE_INPUTS_H

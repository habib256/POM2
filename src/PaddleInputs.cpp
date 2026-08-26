// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "PaddleInputs.h"

namespace pom2 {

PaddleInputs::PaddleInputs()
{
    for (auto& value : values_) value.store(128, std::memory_order_relaxed);
    for (auto& button : buttons_) button.store(false, std::memory_order_relaxed);
}

void PaddleInputs::setPaddle(int index, uint8_t value)
{
    if (index >= 0 && index < static_cast<int>(values_.size()))
        values_[static_cast<size_t>(index)].store(value, std::memory_order_relaxed);
}

void PaddleInputs::setButton(int index, bool down)
{
    if (index >= 0 && index < static_cast<int>(buttons_.size()))
        buttons_[static_cast<size_t>(index)].store(down,
                                                   std::memory_order_relaxed);
}

uint8_t PaddleInputs::read(uint8_t canonicalLow, uint64_t cycle,
                           bool iieMode) const
{
    switch (canonicalLow) {
        case 0x61:
            return (buttons_[0].load(std::memory_order_relaxed) ||
                    openApple_.load(std::memory_order_relaxed)) ? 0x80 : 0x00;
        case 0x62:
            return (buttons_[1].load(std::memory_order_relaxed) ||
                    solidApple_.load(std::memory_order_relaxed)) ? 0x80 : 0x00;
        case 0x63:
            return (buttons_[2].load(std::memory_order_relaxed) ||
                    (iieMode && shift_.load(std::memory_order_relaxed)))
                       ? 0x80 : 0x00;
        case 0x64: case 0x65: case 0x66: case 0x67: {
            const size_t index = canonicalLow - 0x64;
            const uint64_t threshold =
                static_cast<uint64_t>(
                    values_[index].load(std::memory_order_relaxed)) * 11;
            return cycle - latchCycle_ < threshold ? 0x80 : 0x00;
        }
        default:
            return 0;
    }
}

} // namespace pom2

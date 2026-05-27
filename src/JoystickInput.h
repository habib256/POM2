// POM2 Apple II Emulator
// Copyright (C) 2026
//
// JoystickInput — GLFW host-side joystick polling, mapped to the Apple II
// game port. ONE active joystick at a time:
//
//   host X axis  → PADL(0)   ($C064)
//   host Y axis  → PADL(1)   ($C065)
//   host btn 0   → PB0       ($C061, "open-apple")
//   host btn 1   → PB1       ($C062, "closed-apple")
//   host btn 2   → PB2       ($C063, "shift mod" / third button)
//
// PADL(2) and PADL(3) are not driven (return centered, 127). They were
// the second-stick inputs on real hardware and are rarely used.
//
// All 16 GLFW slots are polled each frame so a hot-plugged pad shows up
// in the selection combo immediately. The active "binding" picks one of
// them by index (or -1 for none = paddles read centered, buttons up).

#ifndef POM2_JOYSTICK_INPUT_H
#define POM2_JOYSTICK_INPUT_H

#include <array>
#include <cstdint>
#include <string>

class JoystickInput
{
public:
    static constexpr int kHostCount  = 16;     // GLFW slots
    static constexpr int kAxes       = 2;      // X, Y
    static constexpr int kButtons    = 3;      // PB0, PB1, PB2

    struct DeviceState {
        bool        present = false;
        std::string name;
        std::array<float, kAxes>    axis{};       // -1..+1
        std::array<bool,  kButtons> buttons{};
    };

    struct Binding {
        int   hostIdx  = -1;        // GLFW slot driving the Apple II joy.
                                    // -1 = none (paddles centered, buttons up).
        float deadzone = 0.10f;
        std::array<bool, kAxes> invert{ false, false };
    };

    JoystickInput();

    /// Refresh state from GLFW. Call once per UI frame.
    void poll();

    /// First-call auto-bind: if no host is bound yet and at least one is
    /// present, attach the first present host. Lets the emulator "just
    /// work" without going through the panel.
    void autoBindIfUnconfigured();

    const DeviceState& deviceState(int hostIdx) const {
        if (hostIdx < 0 || hostIdx >= kHostCount) return devices[0];
        return devices[hostIdx];
    }

    Binding&       binding()       { return active; }
    const Binding& binding() const { return active; }

    /// 0..3. Paddles 0/1 reflect the bound host's X/Y; 2/3 are always
    /// centered (no second-stick wiring).
    uint8_t paddleValue(int paddleIdx) const;

    /// 0..2. PB0/PB1/PB2 = bound host buttons 0/1/2.
    bool buttonDown(int buttonIdx) const;

    bool anyPresent() const;

private:
    std::array<DeviceState, kHostCount> devices{};
    Binding active;
    bool    autoBindDone = false;

    static uint8_t axisToPaddle(float axis, float deadzone, bool invert);
};

#endif // POM2_JOYSTICK_INPUT_H

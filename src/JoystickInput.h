// VERHILLE Arnaud 2026

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

    // UI navigation edges, derived from the bound pad's *standard* gamepad
    // mapping (GLFW gamepad state / SDL DB) so the on-screen kiosk disk
    // selector can be driven without a keyboard. Each field is a rising-edge
    // pulse (true for exactly one poll), so a single press = one action.
    // Populated on desktop only; all-false under Emscripten. Distinct from
    // the 3 Apple game-port buttons above: Start/A/B/D-pad are *not* wired to
    // the Apple II, so using them for menus never leaks into gameplay.
    struct UiNav {
        bool menu    = false;   // Start — open/close the selector
        bool up      = false;   // D-pad up   / left-stick up
        bool down    = false;   // D-pad down / left-stick down
        bool confirm = false;   // A — mount the highlighted disk
        bool cancel  = false;   // B — dismiss the selector
    };

    struct Binding {
        int   hostIdx  = -1;        // GLFW slot driving the Apple II joy.
                                    // -1 = none (paddles centered, buttons up).
        float deadzone = 0.10f;
        std::array<bool, kAxes> invert{ false, false };
        // Square-gate emulation. A modern analog stick rides in a ROUND
        // gate, so a full diagonal only reaches (~0.707, ~0.707) and the
        // four extreme corners (full X *and* full Y at once) are physically
        // unreachable. The original Apple II stick rode in a SQUARE gate,
        // where the corners were reachable — which some games (Wings of
        // Fury's take-off) require. When true, paddleValue() expands the
        // circular stick region back out to the full square. See
        // JoystickInput.cpp for the transform.
        bool  squareGate = true;
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

    /// UI-navigation edges from the bound pad's standard gamepad mapping,
    /// refreshed by poll(). Rising-edge pulses (one poll each). All-false
    /// when no gamepad-mapped pad is bound (or under Emscripten).
    const UiNav& uiNav() const { return nav_; }

    /// True when the bound pad exposes a standard GLFW/SDL gamepad mapping
    /// (so Start/A/B/D-pad in uiNav() are meaningful). When false, the pad
    /// is a raw joystick with unknown button layout and uiNav() stays idle.
    bool activeIsGamepad() const { return activeIsGamepad_; }

    /// Name of the currently bound host pad (empty when none / out of range).
    std::string activeName() const {
        if (active.hostIdx < 0 || active.hostIdx >= kHostCount) return {};
        return devices[active.hostIdx].name;
    }

    /// Pure square-gate transform, exposed for unit testing. Takes a raw
    /// stick position (each component in [-1,+1]) and expands the inscribed
    /// circle out to the full square so the corners become reachable; a
    /// point already on an axis is left untouched. In-place.
    static void applySquareGate(float& x, float& y);

    /// Map a cleaned axis value (already inverted / deadzoned / gated,
    /// in [-1,+1]) to an Apple II paddle reading [0,255], 0.0 → 128.
    /// Public for unit testing (pure, side-effect free).
    static uint8_t axisToPaddle01(float axis);

private:
    std::array<DeviceState, kHostCount> devices{};
    Binding active;
    bool    autoBindDone = false;

    // UI-nav edge detection state (desktop only). Previous digital-button
    // snapshot (GLFW gamepad layout: 15 buttons) + previous virtual-stick
    // vertical direction (-1/0/+1) so left-stick pushes fire like a D-pad.
    UiNav                       nav_{};
    std::array<unsigned char, 15> navPrevButtons_{};
    int  navPrevStickY_   = 0;
    bool navPrevValid_    = false;
    bool activeIsGamepad_ = false;   // bound pad has a standard mapping
};

#endif // POM2_JOYSTICK_INPUT_H

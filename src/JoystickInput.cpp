// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "JoystickInput.h"

#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#else
#include <GLFW/glfw3.h>
#endif

#include <cmath>

JoystickInput::JoystickInput() = default;

uint8_t JoystickInput::axisToPaddle(float axis, float deadzone, bool invert)
{
    if (invert) axis = -axis;
    if (std::fabs(axis) < deadzone) axis = 0.0f;
    // Map [-1, +1] → [0, 255]. The +0.5 round-to-nearest puts 0.0 on 128
    // (center, matching paddleValue()'s 128 fallback) and +1.0 on 255
    // cleanly. Clamp guards against drivers briefly
    // returning slightly out-of-range values.
    float v = (axis + 1.0f) * 127.5f;
    if (v < 0.0f)   v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return static_cast<uint8_t>(v + 0.5f);
}

void JoystickInput::poll()
{
#ifdef __EMSCRIPTEN__
    emscripten_sample_gamepad_data();
    const int browserPads = emscripten_get_num_gamepads();
    for (int h = 0; h < kHostCount; ++h) {
        DeviceState& d = devices[h];
        d.present = false;
        d.name.clear();
        d.axis.fill(0.0f);
        d.buttons.fill(false);

        if (browserPads <= 0 || h >= browserPads) continue;

        EmscriptenGamepadEvent gp{};
        if (emscripten_get_gamepad_status(h, &gp) != EMSCRIPTEN_RESULT_SUCCESS ||
            !gp.connected) {
            continue;
        }

        d.present = true;
        d.name = gp.id[0] ? gp.id : "Browser Gamepad";
        for (int i = 0; i < kAxes; ++i) {
            d.axis[i] = (i < gp.numAxes) ? static_cast<float>(gp.axis[i]) : 0.0f;
        }
        for (int i = 0; i < kButtons; ++i) {
            d.buttons[i] = (i < gp.numButtons) &&
                           (gp.digitalButton[i] || gp.analogButton[i] >= 0.5);
        }
    }
#else
    for (int h = 0; h < kHostCount; ++h) {
        const int jid = GLFW_JOYSTICK_1 + h;
        DeviceState& d = devices[h];
        if (!glfwJoystickPresent(jid)) {
            d.present = false;
            d.name.clear();
            d.axis.fill(0.0f);
            d.buttons.fill(false);
            continue;
        }
        d.present = true;
        if (const char* name = glfwGetJoystickName(jid)) d.name = name;
        else d.name = "Unknown";

        int axisCount = 0;
        const float* axes = glfwGetJoystickAxes(jid, &axisCount);
        for (int i = 0; i < kAxes; ++i) {
            d.axis[i] = (axes && i < axisCount) ? axes[i] : 0.0f;
        }

        int btnCount = 0;
        const unsigned char* btns = glfwGetJoystickButtons(jid, &btnCount);
        for (int i = 0; i < kButtons; ++i) {
            d.buttons[i] = (btns && i < btnCount && btns[i] == GLFW_PRESS);
        }
    }
#endif
}

void JoystickInput::autoBindIfUnconfigured()
{
#ifdef __EMSCRIPTEN__
    if (active.hostIdx >= 0 && active.hostIdx < kHostCount &&
        devices[active.hostIdx].present) {
        return;
    }
    for (int h = 0; h < kHostCount; ++h) {
        if (devices[h].present) {
            active.hostIdx = h;
            autoBindDone = true;
            return;
        }
    }
    active.hostIdx = -1;
#else
    if (autoBindDone) return;
    if (active.hostIdx >= 0) return;
    for (int h = 0; h < kHostCount; ++h) {
        if (devices[h].present) {
            active.hostIdx = h;
            autoBindDone = true;
            return;
        }
    }
#endif
}

uint8_t JoystickInput::paddleValue(int paddleIdx) const
{
    // Only paddles 0/1 are driven by the active host; 2/3 are not wired.
    // Return centered = 128 to match both the centered live value
    // (axisToPaddle(0.0) → 128) and Memory's paddleValue default.
    if (paddleIdx < 0 || paddleIdx >= kAxes) return 128;
    if (active.hostIdx < 0 || active.hostIdx >= kHostCount) return 128;
    const DeviceState& d = devices[active.hostIdx];
    if (!d.present) return 128;
    return axisToPaddle(d.axis[paddleIdx],
                        active.deadzone,
                        active.invert[paddleIdx]);
}

bool JoystickInput::buttonDown(int buttonIdx) const
{
    if (buttonIdx < 0 || buttonIdx >= kButtons) return false;
    if (active.hostIdx < 0 || active.hostIdx >= kHostCount) return false;
    const DeviceState& d = devices[active.hostIdx];
    if (!d.present) return false;
    return d.buttons[buttonIdx];
}

bool JoystickInput::anyPresent() const
{
    for (const auto& d : devices) if (d.present) return true;
    return false;
}

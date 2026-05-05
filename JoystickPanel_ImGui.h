// POM2 Apple II Emulator
// Copyright (C) 2026
//
// JoystickPanel_ImGui — single-device joystick configuration panel.
// One combo box picks which host pad feeds the Apple II game port.
// Below: deadzone, invert X/Y, live axis bars + button LEDs, and an
// Apple II side mirror so the user can verify what BASIC will read
// from PADL(...) and the PB$Cs.

#ifndef POM2_JOYSTICK_PANEL_IMGUI_H
#define POM2_JOYSTICK_PANEL_IMGUI_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pom2 {

class JoystickPanel_ImGui
{
public:
    static constexpr int kAxes    = 2;
    static constexpr int kButtons = 3;

    /// One entry per detected GLFW host joystick. Used to populate the
    /// combo and the read-only "all detected devices" overflow list.
    struct HostDevice {
        int         index = -1;
        std::string name;
        std::array<float, kAxes>    axis{};
        std::array<bool,  kButtons> buttons{};
    };

    struct Snapshot {
        std::vector<HostDevice> hosts;

        // Active binding, mirrored from JoystickInput::Binding.
        int   hostIdx  = -1;
        float deadzone = 0.10f;
        std::array<bool, kAxes> invert{ false, false };

        // Live Apple-II-side values for confirmation.
        std::array<uint8_t, 4> appleIIPaddle{ 127, 127, 127, 127 };
        std::array<bool,    3> appleIIButton{ false, false, false };
    };

    struct FrameResult {
        bool  changed  = false;
        int   hostIdx  = -1;
        float deadzone = 0.10f;
        std::array<bool, kAxes> invert{ false, false };
    };

    FrameResult render(const char* title, bool& open, const Snapshot& snap);
};

} // namespace pom2

#endif // POM2_JOYSTICK_PANEL_IMGUI_H

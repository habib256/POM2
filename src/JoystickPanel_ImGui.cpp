// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "JoystickPanel_ImGui.h"

#include "imgui.h"

#include <cstdio>

namespace pom2 {

namespace {

void drawAxisBar(const char* label, float value, float deadzone)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(60);
    const ImVec2 size(180.0f, 14.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();

    dl->AddRectFilled(p0, ImVec2(p0.x + size.x, p0.y + size.y),
                      IM_COL32(40, 40, 40, 255));
    const float cx = p0.x + size.x * 0.5f;
    dl->AddLine(ImVec2(cx, p0.y), ImVec2(cx, p0.y + size.y),
                IM_COL32(90, 90, 90, 255));
    if (deadzone > 0.0f) {
        const float dx = size.x * 0.5f * deadzone;
        dl->AddRectFilled(ImVec2(cx - dx, p0.y),
                          ImVec2(cx + dx, p0.y + size.y),
                          IM_COL32(60, 60, 30, 255));
    }
    float clamped = value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    const float vx = cx + clamped * size.x * 0.5f;
    dl->AddRectFilled(ImVec2(vx - 2.0f, p0.y - 2.0f),
                      ImVec2(vx + 2.0f, p0.y + size.y + 2.0f),
                      IM_COL32(220, 200, 80, 255));
    ImGui::Dummy(size);
    ImGui::SameLine();
    ImGui::Text("%+.2f", value);
}

void drawButtonLed(const char* label, bool down)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(60);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float r = 6.0f;
    const ImU32 col = down ? IM_COL32(220, 60, 60, 255)
                           : IM_COL32(60, 60, 60, 255);
    dl->AddCircleFilled(ImVec2(p.x + r + 2, p.y + r + 2), r, col);
    dl->AddCircle      (ImVec2(p.x + r + 2, p.y + r + 2), r,
                        IM_COL32(0, 0, 0, 255), 0, 1.5f);
    ImGui::Dummy(ImVec2(r * 2 + 8, r * 2 + 4));
    ImGui::SameLine();
    ImGui::TextDisabled(down ? "DOWN" : "up");
}

const JoystickPanel_ImGui::HostDevice*
findHost(const std::vector<JoystickPanel_ImGui::HostDevice>& hosts, int idx)
{
    for (const auto& h : hosts) if (h.index == idx) return &h;
    return nullptr;
}

} // namespace

JoystickPanel_ImGui::FrameResult JoystickPanel_ImGui::render(
    const char* title, bool& open, const Snapshot& snap)
{
    FrameResult r;
    r.hostIdx  = snap.hostIdx;
    r.deadzone = snap.deadzone;
    r.invert   = snap.invert;

    if (!open) return r;

    ImGui::SetNextWindowSize(ImVec2(380, 380), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    ImGui::TextDisabled(
        "Pick which host pad drives the Apple II game port. Its X/Y axes"
        " feed PADL(0)/PADL(1); buttons feed PB0/PB1/PB2.");
    ImGui::Separator();

    // ─── Device combo ────────────────────────────────────────────────────
    auto previewLabel = [&](int idx) -> std::string {
        if (idx < 0) return "(none — joystick disabled)";
        const auto* h = findHost(snap.hosts, idx);
        char buf[160];
        std::snprintf(buf, sizeof(buf), "#%d %s",
                      idx + 1, h ? h->name.c_str() : "(disconnected)");
        return buf;
    };

    if (ImGui::BeginCombo("Joystick", previewLabel(snap.hostIdx).c_str())) {
        if (ImGui::Selectable("(none — joystick disabled)",
                              snap.hostIdx < 0)) {
            r.hostIdx  = -1;
            r.changed  = true;
        }
        for (const auto& h : snap.hosts) {
            char item[192];
            std::snprintf(item, sizeof(item), "#%d %s",
                          h.index + 1, h.name.c_str());
            if (ImGui::Selectable(item, h.index == snap.hostIdx)) {
                r.hostIdx = h.index;
                r.changed = true;
            }
        }
        ImGui::EndCombo();
    }

    if (snap.hosts.empty()) {
        ImGui::TextDisabled("No host pad detected. Plug one in;"
                            " GLFW will pick it up automatically.");
    }

    ImGui::Spacing();

    // ─── Live preview of the bound device ────────────────────────────────
    const HostDevice* bound =
        snap.hostIdx >= 0 ? findHost(snap.hosts, snap.hostIdx) : nullptr;
    if (bound) {
        drawAxisBar("X axis", bound->axis[0], snap.deadzone);
        drawAxisBar("Y axis", bound->axis[1], snap.deadzone);
        ImGui::Spacing();
        drawButtonLed("Btn 0", bound->buttons[0]);
        drawButtonLed("Btn 1", bound->buttons[1]);
        drawButtonLed("Btn 2", bound->buttons[2]);
    } else {
        ImGui::TextDisabled("Joystick disabled — paddles read 127, buttons up.");
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ─── Tuning ──────────────────────────────────────────────────────────
    if (ImGui::SliderFloat("Deadzone", &r.deadzone, 0.0f, 0.5f, "%.2f"))
        r.changed = true;
    if (ImGui::Checkbox("Invert X", &r.invert[0])) r.changed = true;
    ImGui::SameLine();
    if (ImGui::Checkbox("Invert Y", &r.invert[1])) r.changed = true;

    // ─── Apple II side mirror ────────────────────────────────────────────
    ImGui::Separator();
    ImGui::TextUnformatted("Apple II side (live):");
    ImGui::Text(" PADL(0)=%-3u  PADL(1)=%-3u",
                snap.appleIIPaddle[0], snap.appleIIPaddle[1]);
    ImGui::Text(" PB0=%s  PB1=%s  PB2=%s",
                snap.appleIIButton[0] ? "DOWN" : "up",
                snap.appleIIButton[1] ? "DOWN" : "up",
                snap.appleIIButton[2] ? "DOWN" : "up");

    ImGui::End();
    return r;
}

} // namespace pom2

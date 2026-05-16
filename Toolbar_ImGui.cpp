// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Toolbar_ImGui.h"

#include "IconsFontAwesome6.h"
#include "imgui.h"

namespace pom2 {

namespace {

// Convenience: icon + text fallback in one place. When `fa-solid-900.ttf`
// failed to load the icon glyph renders as `?`; the tooltip carries the
// long label either way.
struct Btn { const char* icon; const char* id; const char* tip; };

bool iconButton(const Btn& b, bool enabled = true) {
    ImGui::BeginDisabled(!enabled);
    char label[64];
    std::snprintf(label, sizeof(label), "%s##%s", b.icon, b.id);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && b.tip) ImGui::SetTooltip("%s", b.tip);
    return clicked;
}

const char* profileShortLabel(SystemProfile p) {
    switch (p) {
        case SystemProfile::AppleII:             return "][";
        case SystemProfile::AppleIIPlus:         return "][+";
        case SystemProfile::AppleIIeUnenhanced:  return "//e-U";
        case SystemProfile::AppleIIe:            return "//e";
        case SystemProfile::AppleIIc:            return "//c";
        case SystemProfile::AppleIIcPlus:        return "//c+";
    }
    return "??";
}

} // anon namespace

Toolbar_ImGui::Result Toolbar_ImGui::render(
    bool& open, float /*unused*/, const Snapshot& snap)
{
    Result r;
    if (!open) return r;

    // Pin to the top-left, flush against the menu bar. `WorkPos`
    // already excludes the main menu bar (it's the top-left of the
    // viewport's work area), so we use it as-is — adding a
    // `menuBarHeight` offset on top pushed the toolbar one row too
    // low (fixed 2026-05-15). The `menuBarHeight` parameter is kept
    // for ABI stability in case the caller can't always feed
    // `WorkPos`; ignored here.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, 0));
    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar     |
        ImGuiWindowFlags_NoMove         |
        ImGuiWindowFlags_NoResize       |
        ImGuiWindowFlags_NoScrollbar    |
        ImGuiWindowFlags_NoSavedSettings|
        ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 4.0f));
    if (!ImGui::Begin("##POM2_Toolbar", nullptr, flags)) {
        ImGui::End();
        ImGui::PopStyleVar(2);
        return r;
    }

    // ── Power group ───────────────────────────────────────────────────
    if (iconButton({ ICON_FA_POWER_OFF,    "ColdBoot",
                     "Power-cycle (wipe RAM + cold boot)" })) {
        r.requestColdBoot = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_ROTATE_LEFT,  "SoftReset",
                     "Soft reset (Ctrl-Reset / F11)" })) {
        r.requestSoftReset = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_ROTATE,       "HardReset",
                     "Hard reset (F12 — re-fetch reset vector)" })) {
        r.requestHardReset = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Run group ─────────────────────────────────────────────────────
    const char* runIcon = snap.isRunning
        ? ICON_FA_CIRCLE_PAUSE : ICON_FA_CIRCLE_PLAY;
    const char* runTip  = snap.isRunning
        ? "Pause (CPU stops at next instruction boundary)"
        : "Run (resume CPU from current PC)";
    if (iconButton({ runIcon, "PauseToggle", runTip })) {
        r.requestPauseToggle = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_FORWARD_STEP, "Step",
                     "Single-instruction step (only useful when paused)" },
                   !snap.isRunning)) {
        r.requestStep = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Speed selector ───────────────────────────────────────────────
    // Combo: 1× / 2× / 4× / MAX. The current speed sticks to whichever
    // bucket the cyclesPerFrame value rounds into; off-bucket values
    // (the user typed a custom one in the Emulation panel) read as
    // "custom" with no checkmark.
    static constexpr int kSpeed1x  = 17045;
    static constexpr int kSpeed2x  = 17045 * 2;
    static constexpr int kSpeed4x  = 17045 * 4;
    static constexpr int kSpeedMax = 1'000'000;
    const char* speedLabel = "Speed";
    if      (snap.cyclesPerFrame == kSpeed1x)  speedLabel = ICON_FA_GAUGE_SIMPLE " 1×";
    else if (snap.cyclesPerFrame == kSpeed2x)  speedLabel = ICON_FA_GAUGE      " 2×";
    else if (snap.cyclesPerFrame == kSpeed4x)  speedLabel = ICON_FA_GAUGE_HIGH " 4×";
    else if (snap.cyclesPerFrame == kSpeedMax) speedLabel = ICON_FA_BOLT       " MAX";
    else                                        speedLabel = ICON_FA_GAUGE      " …";
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::BeginCombo("##POM2ToolbarSpeed", speedLabel)) {
        if (ImGui::Selectable(ICON_FA_GAUGE_SIMPLE " 1× (1.02 MHz)",
                              snap.cyclesPerFrame == kSpeed1x))
            r.setCyclesPerFrame = kSpeed1x;
        if (ImGui::Selectable(ICON_FA_GAUGE      " 2× (2.05 MHz)",
                              snap.cyclesPerFrame == kSpeed2x))
            r.setCyclesPerFrame = kSpeed2x;
        if (ImGui::Selectable(ICON_FA_GAUGE_HIGH " 4× (4.09 MHz)",
                              snap.cyclesPerFrame == kSpeed4x))
            r.setCyclesPerFrame = kSpeed4x;
        if (ImGui::Selectable(ICON_FA_BOLT       " MAX (uncapped)",
                              snap.cyclesPerFrame == kSpeedMax))
            r.setCyclesPerFrame = kSpeedMax;
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emulation speed");

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Profile selector ─────────────────────────────────────────────
    // Short-label combo. Switching = full cold reset under the hood
    // (the host calls `applyProfile()`), so the user picks rarely.
    char profileBuf[16];
    std::snprintf(profileBuf, sizeof(profileBuf),
                  ICON_FA_COMPUTER " %s", profileShortLabel(snap.activeProfile));
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::BeginCombo("##POM2ToolbarProfile", profileBuf)) {
        for (SystemProfile p : pom2::allProfiles()) {
            char rowBuf[32];
            std::snprintf(rowBuf, sizeof(rowBuf), "%s  (%s)",
                          profileShortLabel(p),
                          std::string(pom2::profileConfig(p).displayName).c_str());
            if (ImGui::Selectable(rowBuf, snap.activeProfile == p)) {
                r.setProfileRequested = true;
                r.setProfile          = p;
            }
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("System profile (cold-reset switch)");

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Disk shortcuts ───────────────────────────────────────────────
    if (iconButton({ ICON_FA_FLOPPY_DISK,  "InsertDisk",
                     "Insert disk into the primary Disk II (lowest-slot card)" },
                   snap.hasPrimaryDiskCard)) {
        r.requestInsertDisk = true;
    }
    ImGui::SameLine();
    if (iconButton({ ICON_FA_EJECT,        "EjectAll",
                     "Eject every loaded Disk II / HDV / SmartPort image" },
                   snap.hasAnyDiskLoaded)) {
        r.requestEjectAllDisks = true;
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("|");
    ImGui::SameLine();

    // ── Tooling ──────────────────────────────────────────────────────
    if (iconButton({ ICON_FA_CAMERA,       "Screenshot",
                     "Save screenshot to ./screenshot_NNN.ppm  (F9)" })) {
        r.requestScreenshot = true;
    }
    ImGui::SameLine();
    {
        // Memory viewer is a toggle — render the icon with a light tint
        // when the viewer is currently visible so the state is obvious
        // at a glance.
        if (snap.memViewerVisible) {
            ImGui::PushStyleColor(ImGuiCol_Button,
                ImGui::GetStyle().Colors[ImGuiCol_ButtonActive]);
        }
        if (iconButton({ ICON_FA_MEMORY,   "MemViewer",
                         snap.memViewerVisible
                             ? "Memory viewer (visible — click to hide)"
                             : "Memory viewer (click to show)" })) {
            r.requestMemViewerToggle = true;
        }
        if (snap.memViewerVisible) ImGui::PopStyleColor();
    }

    ImGui::End();
    ImGui::PopStyleVar(2);
    return r;
}

} // namespace pom2

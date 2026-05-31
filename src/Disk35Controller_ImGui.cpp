// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Disk35Controller_ImGui.h"

#include "StatusLed.h"
#include "imgui.h"

#include <cstdio>

namespace pom2 {

namespace {

// Motor LED — solid red dot when the drive is spinning. Same widget as
// `DiskController_ImGui` (Disk II), kept private to this TU so the two
// panels don't drift visually.
void drawMotorLed(bool motorOn)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float  radius = 6.0f;
    const ImU32  color  = motorOn
        ? IM_COL32(220, 60, 60, 255)
        : IM_COL32(60, 60, 60, 255);
    dl->AddCircleFilled(ImVec2(p.x + radius + 4, p.y + radius + 2), radius, color);
    dl->AddCircle      (ImVec2(p.x + radius + 4, p.y + radius + 2), radius,
                        IM_COL32(0, 0, 0, 255), 0, 1.5f);
    ImGui::Dummy(ImVec2(radius * 2 + 12, radius * 2 + 4));
}

// One drive block — motor LED + status text + Mount / Eject / write-back
// row. Returns nothing; user actions are written straight to `r`.
void renderDriveBlock(int                                       driveIdx,
                      const char*                               driveName,
                      const Disk35Controller_ImGui::DriveSnapshot& drv,
                      Disk35Controller_ImGui::FrameResult&      r)
{
    ImGui::PushID(driveIdx);

    pom2::statusLed(drv.diskLoaded, drv.writeProtected,
                    /*error=*/(!drv.diskLoaded && !drv.lastError.empty()),
                    drv.diskLoaded ? drv.diskPath.c_str() : nullptr);
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "%s", driveName);
    ImGui::SameLine();
    drawMotorLed(drv.motorOn);
    ImGui::SameLine();
    ImGui::TextDisabled(drv.motorOn ? "MOTOR" : "idle");

    if (drv.diskLoaded) {
        ImGui::Text("Track: %2d   Head %d   %s",
                    drv.track, drv.side1 ? 1 : 0,
                    drv.writeProtected ? "(write-protected)" : "");
        ImGui::TextWrapped("Image: %s", drv.diskPath.c_str());
    } else {
        ImGui::TextDisabled("No disk inserted.");
        if (!drv.lastError.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(220, 70, 70, 255));
            ImGui::TextWrapped("Last error: %s", drv.lastError.c_str());
            ImGui::PopStyleColor();
        }
    }

    // ── Buttons row ─────────────────────────────────────────────────────
    if (ImGui::Button("Mount .po / .2mg...")) {
        r.openMountDialog         = true;
        r.openMountDialogForDrive = driveIdx;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!drv.diskLoaded);
    if (ImGui::Button("Eject")) {
        r.requestEject[driveIdx] = true;
    }
    ImGui::EndDisabled();

    // ── Write-back checkbox — mirrors Disk II ──────────────────────────
    bool writeBack = drv.writeBackEnabled;
    if (ImGui::Checkbox("Write-back (save on eject)", &writeBack)) {
        r.requestWriteBackToggle[driveIdx] = true;
        r.newWriteBack[driveIdx]           = writeBack;
    }
    if (drv.hasUnsavedChanges) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(unsaved)");
    }

    ImGui::PopID();
}

} // anon namespace

Disk35Controller_ImGui::FrameResult Disk35Controller_ImGui::render(
    const char*          title,
    bool&                open,
    const PanelSnapshot& snap)
{
    FrameResult r;
    if (!open) return r;

    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    if (!snap.supportedByProfile) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                           "Sony 3.5\" drives are only active on the");
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f),
                           "Apple //c+ profile (Hardware → Presets).");
        ImGui::Separator();
    }

    static const char* kDriveNames[2] = {
        "Drive 1 (internal — //c+ on-board)",
        "Drive 2 (external — SmartPort daisy-chain)",
    };

    for (int d = 0; d < 2; ++d) {
        renderDriveBlock(d, kDriveNames[d], snap.drives[d], r);
        ImGui::Separator();
    }

    // ─── Library ────────────────────────────────────────────────────────
    // Same UX as the Disk II library: left-click = mount + cold-boot into
    // drive 1, right-click opens a context menu offering "Mount only" on
    // either drive. Currently-inserted images are prefixed with `* ` so a
    // re-click is recognisable.
    ImGui::TextUnformatted("Library:");
    ImGui::SameLine();
    ImGui::TextDisabled("(left-click: mount drive 1 + boot — "
                        "right-click: more options)");

    if (snap.library.empty()) {
        ImGui::TextDisabled("  (drop .po / .2mg files into disks_3.5/ to populate)");
    } else {
        // Match the Disk II library height — same panel layout, same
        // breathing room for one-click access without scrolling unless
        // the collection is unusually large.
        ImGui::BeginChild("##disk35_lib", ImVec2(0, 540), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry : snap.library) {
            const bool currentDrv0 = (entry.fullPath == snap.drives[0].diskPath);
            const bool currentDrv1 = (entry.fullPath == snap.drives[1].diskPath);
            const bool current = currentDrv0 || currentDrv1;
            // Visual marker for currently-inserted images. We don't try
            // to distinguish drive 1 vs drive 2 in the prefix — the
            // context menu makes that explicit if the user needs to act
            // on a specific drive.
            const std::string label = (current ? "* " : "  ") + entry.displayName;

            ImGui::PushID(entry.fullPath.c_str());
            if (ImGui::Selectable(label.c_str(), current)) {
                // Left-click default: mount into drive 1 + cold-boot.
                r.requestInsertAndBoot = entry.fullPath;
                r.insertAndBootDrive   = 0;
            }
            if (ImGui::BeginPopupContextItem("ctx")) {
                if (ImGui::MenuItem("Mount on drive 1 + boot")) {
                    r.requestInsertAndBoot = entry.fullPath;
                    r.insertAndBootDrive   = 0;
                }
                if (ImGui::MenuItem("Mount on drive 1 (no boot)")) {
                    r.requestMountPath  = entry.fullPath;
                    r.requestMountDrive = 0;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Mount on drive 2 + boot")) {
                    r.requestInsertAndBoot = entry.fullPath;
                    r.insertAndBootDrive   = 1;
                }
                if (ImGui::MenuItem("Mount on drive 2 (no boot)")) {
                    r.requestMountPath  = entry.fullPath;
                    r.requestMountDrive = 1;
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return r;
}

} // namespace pom2

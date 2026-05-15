// POM2 Apple II Emulator
// Copyright (C) 2026

#include "Disk35Controller_ImGui.h"

#include "imgui.h"

namespace pom2 {

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
        "Internal 3.5\" (slot 6, MIG intdrive)",
        "External 3.5\" (SmartPort daisy-chain)",
    };

    for (int d = 0; d < 2; ++d) {
        const DriveSnapshot& drv = snap.drives[d];
        ImGui::PushID(d);
        ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f), "%s", kDriveNames[d]);

        if (drv.diskLoaded) {
            ImGui::TextWrapped("Image: %s", drv.diskPath.c_str());
            ImGui::Text("Track %2d   Head %d   %s",
                        drv.track, drv.side1 ? 1 : 0,
                        drv.writeProtected ? "(write-protected)" : "");
            ImGui::Text("Motor: %s", drv.motorOn ? "ON" : "off");
        } else {
            ImGui::TextDisabled("No disk inserted.");
            if (!drv.lastError.empty()) {
                ImGui::TextColored(ImVec4(0.95f, 0.4f, 0.4f, 1.0f),
                                   "Last error: %s", drv.lastError.c_str());
            }
        }

        if (ImGui::Button("Mount .po / .2mg...")) {
            r.openMountDialog          = true;
            r.openMountDialogForDrive  = d;
        }
        ImGui::SameLine();
        if (ImGui::Button("Eject")) {
            r.requestEject[d] = true;
        }

        ImGui::Separator();
        ImGui::PopID();
    }

    // ─── Library ─────────────────────────────────────────────────────────
    // The host populates this from `disks35/` (or whatever the user has
    // configured). One click on a row mounts into the *internal* drive
    // (most common case); right-click pops a menu offering the external
    // drive too.
    if (!snap.library.empty()) {
        ImGui::TextDisabled("Library (left-click → internal, right-click → external):");
        ImGui::BeginChild("disk35_lib", ImVec2(0, 120), true);
        for (const auto& e : snap.library) {
            ImGui::PushID(e.fullPath.c_str());
            if (ImGui::Selectable(e.displayName.c_str())) {
                r.requestMountPath  = e.fullPath;
                r.requestMountDrive = 0;
            }
            if (ImGui::BeginPopupContextItem("ctx")) {
                if (ImGui::MenuItem("Mount on internal drive")) {
                    r.requestMountPath  = e.fullPath;
                    r.requestMountDrive = 0;
                }
                if (ImGui::MenuItem("Mount on external drive")) {
                    r.requestMountPath  = e.fullPath;
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

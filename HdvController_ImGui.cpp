// POM2 Apple II Emulator
// Copyright (C) 2026

#include "HdvController_ImGui.h"

#include "imgui.h"

namespace pom2 {

HdvController_ImGui::FrameResult HdvController_ImGui::render(
    const char*          title,
    bool&                open,
    const DriveSnapshot& snap)
{
    FrameResult r;
    if (!open) return r;

    if (!ImGui::Begin(title, &open)) {
        ImGui::End();
        return r;
    }

    // ─── Card status ────────────────────────────────────────────────────
    // The ProDOS block-device ROM is built into the card (no external
    // .rom file to load) — flag it so the user knows nothing else is
    // needed for slot 5 to work.
    ImGui::TextColored(ImVec4(0.4f, 0.85f, 0.4f, 1.0f),
                       "Boot ROM: built-in (slot 5, $C500-$C5FF)");
    ImGui::Separator();

    // ─── Mounted image ───────────────────────────────────────────────────
    if (snap.imageLoaded) {
        ImGui::TextWrapped("Image: %s", snap.imagePath.c_str());
        ImGui::Text("Blocks: %zu (~%.1f MB)",
                    snap.blockCount,
                    (snap.blockCount * 512.0) / (1024.0 * 1024.0));
        if (snap.supportsWriteBack) {
            bool wb = snap.writeBackEnabled;
            const char* lbl = snap.isSynthVolume
                ? "Sync to host folder (on eject)"
                : "Write-back (save on eject)";
            if (ImGui::Checkbox(lbl, &wb)) {
                r.writeBackToggleChanged = true;
                r.writeBackNewValue      = wb;
            }
            if (snap.hasUnsavedChanges) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "(unsaved)");
            }
        } else {
            ImGui::TextDisabled("(read-only — 2MG WP flag set)");
        }
    } else {
        ImGui::TextDisabled("No image mounted.");
    }

    ImGui::Separator();

    // ─── Buttons ─────────────────────────────────────────────────────────
    if (ImGui::Button("Mount .hdv / .2mg...")) {
        mountDialogOpen = true;
        if (dialogPath.empty()) dialogPath = "hdv/";
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!snap.imageLoaded);
    if (ImGui::Button("Eject")) {
        r.requestEject = true;
    }
    ImGui::EndDisabled();

    ImGui::Separator();

    // Direct boot — bypasses any host-side weirdness in PR#5 dispatch.
    // Sets PC = $C500 and lets the slot-5 boot loader take over.
    ImGui::BeginDisabled(!snap.imageLoaded);
    if (ImGui::Button("Boot HDV (jump $C500)")) {
        r.requestBoot = true;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("or type PR#5 in Applesoft");

    // ─── HDV library ─────────────────────────────────────────────────────
    // One-click mount + cold-boot for any .hdv / .2mg in hdv/.
    ImGui::Separator();
    ImGui::TextUnformatted("Library:");
    ImGui::SameLine();
    ImGui::TextDisabled("(left-click: mount + boot — right-click: mount only)");

    if (snap.library.empty()) {
        ImGui::TextDisabled("  (drop .hdv / .2mg files into hdv/ to populate)");
    } else {
        ImGui::BeginChild("##hdv_lib", ImVec2(0, 540), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& entry : snap.library) {
            const bool current = (entry.fullPath == snap.imagePath);
            const std::string label = (current ? "* " : "  ") + entry.displayName;
            if (ImGui::Selectable(label.c_str(), current)) {
                r.requestMountAndBoot = entry.fullPath;
            }
            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                r.requestMountOnly = entry.fullPath;
            }
        }
        ImGui::EndChild();
    }

    ImGui::End();
    return r;
}

} // namespace pom2

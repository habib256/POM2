// POM2 Apple II Emulator
// Copyright (C) 2026
//
// FloppyEmu_ImGui — see header. Stylised OLED + PREV/NEXT/SELECT buttons.

#include "FloppyEmu_ImGui.h"

#include "imgui.h"

#include <algorithm>

namespace pom2 {

namespace {
// Floppy Emu OLED palette: cyan-on-near-black, with an inverse highlight bar.
const ImVec4 kOledBg     (0.02f, 0.03f, 0.10f, 1.0f);
const ImVec4 kOledText   (0.50f, 0.78f, 1.00f, 1.0f);
const ImVec4 kOledDim    (0.32f, 0.50f, 0.70f, 1.0f);
const ImU32  kOledHi   = IM_COL32(70, 140, 220, 255);   // selection bar
const ImVec4 kOledHiTxt  (0.02f, 0.03f, 0.10f, 1.0f);   // text on the bar

int clampCursor(int c, int n) { return n <= 0 ? 0 : std::max(0, std::min(c, n - 1)); }
} // namespace

FloppyEmu_ImGui::Result
FloppyEmu_ImGui::render(const char* title, bool& open, const Snapshot& snap)
{
    Result r;

    ImGui::SetNextWindowSize(ImVec2(440, 430), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) { ImGui::End(); return r; }

    const int nItems = static_cast<int>(snap.items.size());
    const int nModes = static_cast<int>(snap.modeOptions.size());
    browseCursor_   = clampCursor(browseCursor_, nItems);
    settingsCursor_ = clampCursor(settingsCursor_, nModes);

    // Keyboard shortcuts when the window is focused (faithful PREV/NEXT/SELECT).
    const bool focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    bool kPrev = false, kNext = false, kSelect = false;
    if (focused) {
        kPrev   = ImGui::IsKeyPressed(ImGuiKey_UpArrow);
        kNext   = ImGui::IsKeyPressed(ImGuiKey_DownArrow);
        kSelect = ImGui::IsKeyPressed(ImGuiKey_Enter) ||
                  ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
    }

    // ── The OLED ─────────────────────────────────────────────────────────
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kOledBg);
    ImGui::PushStyleColor(ImGuiCol_Text,    kOledText);
    ImGui::PushStyleColor(ImGuiCol_Header,        kOledHi);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, kOledHi);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  kOledHi);
    ImGui::BeginChild("##oled", ImVec2(0, 250), true);

    if (showSettings_) {
        ImGui::TextColored(kOledDim, "DISK EMULATION MODE");
        ImGui::Separator();
        for (int i = 0; i < nModes; ++i) {
            const bool sel = (i == settingsCursor_);
            ImGui::PushID(i);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kOledHiTxt);
            if (ImGui::Selectable(snap.modeOptions[i].c_str(), sel)) {
                settingsCursor_ = i;
                r.setModeIndex  = i;
                showSettings_   = false;
            }
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    } else {
        // Header: device name + current mode (mirrors the title screen).
        ImGui::TextColored(kOledDim, "FLOPPY EMU");
        ImGui::SameLine();
        ImGui::TextColored(kOledText, "%s", snap.modeLabel.c_str());
        ImGui::TextColored(kOledDim, "%s%s",
            snap.favoritesActive ? "\xE2\x98\x85 Favorites" : "SD:",
            snap.favoritesActive ? "" : snap.dirLabel.c_str());
        ImGui::Separator();

        if (!snap.sdPresent) {
            ImGui::TextColored(kOledDim, "No SD card.");
            ImGui::TextColored(kOledDim, "Set the SD folder below.");
        } else if (nItems == 0) {
            ImGui::TextColored(kOledDim, "(no disk images here)");
        }
        for (int i = 0; i < nItems; ++i) {
            const Item& it = snap.items[i];
            const bool sel = (i == browseCursor_);
            ImGui::PushID(i);
            if (sel) ImGui::PushStyleColor(ImGuiCol_Text, kOledHiTxt);
            std::string label = it.isUp ? ".."
                              : it.isDir ? ("[" + it.label + "]")
                              : it.label;
            if (ImGui::Selectable(label.c_str(), sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                browseCursor_  = i;
                r.activateIndex = i;
            }
            if (!it.sublabel.empty()) {
                ImGui::SameLine();
                float w = ImGui::GetContentRegionAvail().x;
                ImGui::SameLine(ImGui::GetCursorPosX() + w - 48.0f);
                ImGui::TextColored(sel ? kOledHiTxt : kOledDim, "%s",
                                   it.sublabel.c_str());
            }
            if (sel) ImGui::PopStyleColor();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(5);

    // ── Hardware buttons: PREV / NEXT / SELECT ───────────────────────────
    const float bw = (ImGui::GetContentRegionAvail().x - 16.0f) / 3.0f;
    int* cursor = showSettings_ ? &settingsCursor_ : &browseCursor_;
    const int   nCur = showSettings_ ? nModes : nItems;
    if (ImGui::Button("\xE2\x97\x80 PREV", ImVec2(bw, 0)) || kPrev)
        *cursor = clampCursor(*cursor - 1, nCur);
    ImGui::SameLine();
    if (ImGui::Button("NEXT \xE2\x96\xB6", ImVec2(bw, 0)) || kNext)
        *cursor = clampCursor(*cursor + 1, nCur);
    ImGui::SameLine();
    if (ImGui::Button("SELECT", ImVec2(bw, 0)) || kSelect) {
        if (showSettings_) {
            if (nModes > 0) { r.setModeIndex = settingsCursor_; showSettings_ = false; }
        } else if (nItems > 0) {
            r.activateIndex = browseCursor_;
        }
    }

    // ── Context buttons ──────────────────────────────────────────────────
    if (!showSettings_) {
        if (ImGui::Button("Settings")) showSettings_ = true;
        if (snap.favoritesAvailable) {
            ImGui::SameLine();
            if (ImGui::Button(snap.favoritesActive ? "All Disks" : "Favorites"))
                r.toggleFavorites = true;
        }
        if (!snap.insertedLabel.empty()) {
            ImGui::SameLine();
            if (ImGui::Button("Eject")) r.requestEject = true;
        }
    } else {
        if (ImGui::Button("Back")) showSettings_ = false;
    }

    // ── Host-side status (below the device) ──────────────────────────────
    ImGui::Separator();
    if (!snap.controllerReady) {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.3f, 1.0f), "%s",
                           snap.controllerHint.c_str());
        if (ImGui::Button("Auto-configure controller"))
            r.requestConfigureController = true;
    }
    if (!snap.insertedLabel.empty())
        ImGui::TextDisabled("Inserted: %s", snap.insertedLabel.c_str());
    if (!snap.sdRootDisplay.empty())
        ImGui::TextDisabled("SD folder: %s/  (drop disk images here)",
                            snap.sdRootDisplay.c_str());
    if (!snap.statusLine.empty())
        ImGui::TextWrapped("%s", snap.statusLine.c_str());

    ImGui::End();
    return r;
}

} // namespace pom2

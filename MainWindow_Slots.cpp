// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MainWindow_Slots — Slot Configuration panel.
//
// Renders an ImGui dialog under Hardware → Slot Configuration that lets
// the user assign one of {Disk II, ProDOS HDV, Super Serial, Clock,
// Le Chat Mauve, Mouse} to each of the 7 expansion slots, or leave a
// slot empty. The selection is persisted to settings as `slot_N_card`
// keys; clicking Apply triggers a controlled restart of the emulation
// thread, which:
//
//   1. Stops the worker (controller.stop()).
//   2. Tears down the SlotBus via `clear()` (each card's onUnplug runs).
//   3. Re-runs `plugSlotsFromSettings()` so the new mapping takes effect.
//   4. Hard-resets the CPU (so PC lands on the new ROM's reset vector).
//   5. Re-starts the worker.
//
// Validation: each card type can only be assigned to one slot at a time.
// Duplicate selections are highlighted in red and Apply stays disabled.
// Mouse Card additionally requires both Apple ROMs to be present —
// otherwise the entry is greyed out in the dropdown.

#include "MainWindow.h"
#include "Logger.h"

#include "imgui.h"

#include <array>
#include <filesystem>

namespace {

// Card types the user can pick for any slot.
struct CardType {
    const char* key;
    const char* label;
};

constexpr CardType kCardTypes[] = {
    { "",          "(empty)"           },
    { "diskii",    "Disk II"           },
    { "hdv",       "ProDOS HDV"        },
    { "ssc",       "Super Serial"      },
    { "clock",     "Clock (ProDOS)"    },
    { "chatmauve", "Le Chat Mauve"     },
    { "mouse",     "Mouse Interface"   },
};

bool mouseRomsPresent()
{
    namespace fs = std::filesystem;
    bool slotRom = false, mcuRom = false;
    for (const char* p : { "roms/mouse_341-0270-c.bin",
                           "../roms/mouse_341-0270-c.bin",
                           "../../roms/mouse_341-0270-c.bin" }) {
        if (fs::exists(p)) { slotRom = true; break; }
    }
    for (const char* p : { "roms/mouse_341-0269.bin",
                           "../roms/mouse_341-0269.bin",
                           "../../roms/mouse_341-0269.bin" }) {
        if (fs::exists(p)) { mcuRom = true; break; }
    }
    return slotRom && mcuRom;
}

}  // namespace

void MainWindow::renderSlotConfigPanel()
{
    if (!showSlotConfigPanel) return;

    ImGui::SetNextWindowSize(ImVec2(440, 360), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Slot Configuration", &showSlotConfigPanel)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "Assign a card type to each Apple II expansion slot. Click Apply "
        "to restart the emulator with the new layout. Each card type may "
        "appear in at most one slot.");
    ImGui::Spacing();

    // Snapshot the current canonical mapping into a working copy so the
    // user's edits are local until Apply.
    static std::array<std::string, 8> draft{};
    static bool draftInited = false;
    if (!draftInited) {
        for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
        draftInited = true;
    }

    const bool mouseAvailable = mouseRomsPresent();

    // ── Per-slot dropdowns ────────────────────────────────────────────
    auto isDuplicate = [&](int slot) -> bool {
        if (draft[slot].empty()) return false;
        for (int s = 1; s <= 7; ++s) {
            if (s != slot && draft[s] == draft[slot]) return true;
        }
        return false;
    };

    bool anyDuplicate = false;
    for (int s = 1; s <= 7; ++s) {
        const bool dup = isDuplicate(s);
        if (dup) anyDuplicate = true;

        // Resolve current card-type label for the combo's preview.
        const char* preview = "(empty)";
        for (const auto& ct : kCardTypes) {
            if (ct.key == draft[s]) { preview = ct.label; break; }
        }

        if (dup) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
        char label[32];
        std::snprintf(label, sizeof(label), "Slot %d", s);
        if (ImGui::BeginCombo(label, preview)) {
            for (const auto& ct : kCardTypes) {
                const bool selected = (ct.key == draft[s]);
                const bool disabled = (std::string(ct.key) == "mouse")
                                       && !mouseAvailable;
                if (disabled) ImGui::BeginDisabled();
                if (ImGui::Selectable(ct.label, selected)) {
                    draft[s] = ct.key;
                }
                if (disabled) {
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::TextDisabled("(ROMs missing)");
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (dup) ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::Separator();

    // ── ROM presence diagnostics ──────────────────────────────────────
    if (mouseAvailable) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                           "Mouse ROMs found.");
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                           "Mouse ROMs missing — the Mouse Interface entry is "
                           "disabled. Place roms/mouse_341-0270-c.bin and "
                           "roms/mouse_341-0269.bin to enable.");
    }
    if (!mouseRomStatus.empty()) {
        ImGui::TextWrapped("Mouse: %s", mouseRomStatus.c_str());
    }

    ImGui::Spacing();

    // ── Action buttons ────────────────────────────────────────────────
    if (anyDuplicate) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                           "One card type per slot — fix duplicates above.");
    }

    ImGui::BeginDisabled(anyDuplicate);
    if (ImGui::Button("Apply (restarts emulator)")) {
        // Persist the draft to settings.
        for (int s = 1; s <= 7; ++s) {
            settings.setString("slot_" + std::to_string(s) + "_card", draft[s]);
        }
        settings.save();
        restartEmulationFromSettings();
        // Re-seed the draft from the live state (it should match what we
        // just wrote, but roundtripping confirms).
        for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Revert")) {
        for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
    }

    ImGui::End();
}

// ─── Emulation restart ──────────────────────────────────────────────────

void MainWindow::restartEmulationFromSettings()
{
    // 1. Stop the worker thread so card destructors don't race against a
    //    running CPU step.
    controller.stop();

    // 2. Tear down all cards and clear our raw pointers. Holding the
    //    state mutex isn't strictly necessary now that the worker is
    //    stopped, but it's cheap insurance against any UI thread that
    //    might be peeking.
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        diskCard      = nullptr;
        hdvCard       = nullptr;
        chatMauveCard = nullptr;
        sscCard       = nullptr;
        clockCard     = nullptr;
        mouseCard     = nullptr;
        controller.memory().slotBus().clear();
        // Also drop any cached display.setChatMauveCard pointer — the
        // next plug call will set it again.
        display.setChatMauveCard(nullptr);
    }

    // 3. Re-run plugSlotsFromSettings() with the freshly-saved keys.
    plugSlotsFromSettings();

    // 4. Restore Disk II disk_path (matches MainWindow ctor's behaviour).
    if (diskCard) {
        diskCard->setWriteBackEnabled(settings.getBool("disk_writeback", false));
        const std::string diskPath = settings.getString("disk_path", "");
        std::error_code ec;
        if (!diskPath.empty() &&
            std::filesystem::is_regular_file(diskPath, ec)) {
            (void)diskCard->insertDisk(diskPath);
        }
    }

    // 5. Hard reset + restart worker.
    controller.cpu().hardReset();
    controller.memory().slotBus().reset();
    controller.start();

    pom2::log().info("Slots", "Emulator restarted with new slot mapping.");
}

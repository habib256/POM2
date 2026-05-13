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
    { "",             "(empty)"           },
    { "diskii",       "Disk II"           },
    { "hdv",          "ProDOS HDV"        },
    { "ssc",          "Super Serial"      },
    { "clock",        "Clock (ProDOS)"    },
    { "chatmauve",    "Le Chat Mauve"     },
    { "mouse",        "Mouse Interface"   },
    { "mockingboard", "Mockingboard A/C"  },
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

std::string MainWindow::firstExistingPath(const std::vector<std::string>& candidates)
{
    namespace fs = std::filesystem;
    for (const auto& p : candidates) {
        std::error_code ec;
        if (!p.empty() && fs::exists(p, ec)) return p;
    }
    // Also probe build-time relative paths (`../`, `../../`) so the
    // emulator works whether launched from the repo root or `build/`.
    for (const auto& p : candidates) {
        std::error_code ec;
        const std::string up1 = "../" + p;
        if (fs::exists(up1, ec)) return up1;
        const std::string up2 = "../../" + p;
        if (fs::exists(up2, ec)) return up2;
    }
    return {};
}

M6502::CpuMode MainWindow::resolveCpuMode(M6502::CpuMode profileDefault) const
{
    const std::string override = settings.getString("cpu_mode_override", "auto");
    if (override == "nmos")  return M6502::CpuMode::NMOS;
    if (override == "65c02") return M6502::CpuMode::CMOS;
    return profileDefault;     // "auto" (default) — follow the profile
}

void MainWindow::applyProfile(pom2::SystemProfile p)
{
    const auto& cfg = pom2::profileConfig(p);
    pom2::log().info("Profile",
        std::string("Switching to ") + std::string(cfg.displayName));

    // 1. Stop the worker thread (cards' destructors must not race a
    //    running CPU step or worker idle-loop probe).
    controller.stop();

    // 2. Snapshot the currently-mounted media so we can re-mount after
    //    the cold reset. The user wants to test the same disk under
    //    different machine models; everything else (CPU state, RAM,
    //    soft switches) is wiped intentionally.
    std::string savedDiskPath, savedHdvPath;
    if (diskCard) {
        // DiskIICard has insertDisk(path) operating on the active drive
        // (default 0); we approximate by reading the slot 6 settings.
        // The legitimate disk-path setting is the source of truth.
        savedDiskPath = settings.getString("disk_path", "");
    }
    if (hdvCard) {
        savedHdvPath = settings.getString("hdv_path", "");
    }

    // 3. Tear down all slot cards under the state mutex. Mockingboard's
    //    AudioSource must be detached BEFORE the slot bus destroys the
    //    card (the audio thread's next callback would dereference a
    //    freed source otherwise — same gotcha as restartEmulationFromSettings).
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        if (mockingboardCard && controller.audio().isAvailable()) {
            controller.audio().removeSource(mockingboardCard->audioSource());
        }
        diskCard         = nullptr;
        hdvCard          = nullptr;
        chatMauveCard    = nullptr;
        sscCard          = nullptr;
        clockCard        = nullptr;
        mouseCard        = nullptr;
        mockingboardCard = nullptr;
        controller.memory().slotBus().clear();
        display.setChatMauveCard(nullptr);

        // 4. Cold-reset memory: wipe user RAM, aux RAM (if IIe), LC banks,
        //    soft switches. The internal IIe IO ROM is re-loaded together
        //    with the main ROM below (loadAppleIIRom slots the 4 KB IO ROM
        //    into Memory::internalIORom).
        controller.memory().clearRam();
        controller.memory().resetSoftSwitches();
        // Flip IIe paging FIRST — loadAppleIIRom's path depends on this:
        // when `iieMode == true` and the dump is 16/32 KB, the loader
        // also populates `internalIORom`. Calling setIIEMode after the
        // load would leave the internal IO ROM in whatever state the
        // previous profile left it.
        controller.memory().setIIEMode(cfg.iieMode);
    }

    // 5. Resolve and load the new main ROM.
    const std::string newRomPath = firstExistingPath(cfg.romProbeOrder);
    if (!newRomPath.empty()
        && controller.memory().loadAppleIIRom(newRomPath.c_str())) {
        romPath  = newRomPath;
        romStatus = std::string(cfg.iieMode ? "IIe/IIc: " : "loaded: ") + newRomPath;
    } else {
        romStatus = std::string("NO ROM (") + cfg.romProbeOrder.front() +
                    " not found) — $D000-$FFFF stub only";
        pom2::log().warn("Profile", romStatus);
    }

    // 6. Char ROM. Always try at least one candidate; loadCharRom returns
    //    a falsey value when the file is missing but Apple2Display falls
    //    back to its built-in 5×7 ASCII font in that case.
    const std::string newCharPath = firstExistingPath(cfg.charRomProbeOrder);
    charRomPath = newCharPath;
    if (!newCharPath.empty()) {
        controller.memory().loadCharRom(newCharPath.c_str());
    }
    if (cfg.iieMode) display.setAuxMemory(controller.memory().auxData());
    else             display.setAuxMemory(nullptr);

    // 7. Re-plug slot cards. plugSlotsFromSettings honours user's
    //    persisted slot config; the profile choice doesn't override that
    //    (e.g. a user who put SSC in slot 4 keeps it across profile
    //    switches).
    plugSlotsFromSettings();

    // 8. Re-mount preserved media.
    if (diskCard && !savedDiskPath.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(savedDiskPath, ec)) {
            (void)diskCard->insertDisk(savedDiskPath);
        }
    }
    if (hdvCard && !savedHdvPath.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(savedHdvPath, ec)) {
            (void)hdvCard->loadImage(savedHdvPath);
        }
    }

    // 9. CPU mode (profile default with optional user override).
    {
        std::lock_guard<std::mutex> lk(controller.stateMutex());
        controller.cpu().setCpuMode(resolveCpuMode(cfg.defaultCpu));
    }

    // 10. Default CPU pacing.
    controller.setCyclesPerFrame(cfg.defaultCyclesPerFrame);

    // 11. Final hard reset — CPU re-fetches PC from the new ROM's reset
    //     vector at $FFFC/$FFFD.
    controller.hardReset();
    controller.start();

    // 12. Persist the profile choice for the next launch.
    activeProfile = p;
    settings.setString("system_profile", std::string(cfg.key));
    settings.save();

    pom2::log().info("Profile",
        std::string("Active = ") + std::string(cfg.displayName) +
        ", ROM = " + (newRomPath.empty() ? "<missing>" : newRomPath) +
        ", CPU = " +
        (controller.cpu().getCpuMode() == M6502::CpuMode::CMOS ? "65C02" : "NMOS"));

    // Re-bind the AI control server to the freshly rebuilt slot pointers.
    // (Profile switch rebuilds the SlotBus; diskCard/hdvCard pointers from
    // the previous profile are stale.)
    aiServer.attach(&controller, &display, diskCard, hdvCard);
    aiServer.setProfileLabel(std::string(cfg.displayName));
}

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
        // Mockingboard's AudioSource lives inside the card. We must
        // unregister it from the audio device BEFORE the slot bus
        // destroys the card, otherwise the audio thread's next callback
        // would dereference a freed source.
        if (mockingboardCard && controller.audio().isAvailable()) {
            controller.audio().removeSource(mockingboardCard->audioSource());
        }
        diskCard         = nullptr;
        hdvCard          = nullptr;
        chatMauveCard    = nullptr;
        sscCard          = nullptr;
        clockCard        = nullptr;
        mouseCard        = nullptr;
        mockingboardCard = nullptr;
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

    // 6. Re-attach the AI control server with the freshly rebuilt card
    //    pointers — the slot-bus tear-down above invalidated whatever
    //    diskCard/hdvCard the server was holding.
    aiServer.attach(&controller, &display, diskCard, hdvCard);

    pom2::log().info("Slots", "Emulator restarted with new slot mapping.");
}

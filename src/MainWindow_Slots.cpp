// VERHILLE Arnaud 2026

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
//   1. Stops the worker (controller->stop()).
//   2. Runs SlotRebuildCoordinator's ordered consumer/topology teardown.
//   3. Re-runs `plugSlotsFromSettings()` so the new mapping takes effect.
//   4. Cold-boots the replacement hardware.
//   5. Re-starts the worker.
//
// Validation: ordinary card types can only be assigned once; storage cards
// explicitly supporting multiple instances retain per-slot state.
// Mouse Card additionally requires both Apple ROMs to be present —
// otherwise the entry is greyed out in the dropdown.

#include "MainWindow.h"
#include "MainWindowUiState.h"
#include "DevicePanelCoordinator.h"
#include "NetworkCoordinator.h"
#include "PrinterCoordinator.h"
#include "SlotConfigurationCoordinator.h"
#include "SlotRebuildCoordinator.h"
#include "StorageCoordinator.h"

// Same heavy-includes-here pattern as MainWindow.cpp — MainWindow.h
// forward-declares the controller / cards / panels.
#include "AiControlServer.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CffaCard.h"
#include "CharRomCatalog.h"
#include "FujiNetHost.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "EmulationController.h"
#include "LeChatMauveCard.h"
#include "Logger.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "PhasorCard.h"
#include "ProDOSHardDiskCard.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotCardCatalog.h"
#include "StatusLed.h"
#include "IconsFontAwesome6.h"
#include "Pom2Theme.h"   // palette() for the staged-change accent
#include "MountableMediaCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"

#include "imgui.h"
#include <GLFW/glfw3.h>

#include <array>
#include <filesystem>

// Card catalog + ROM-presence probes now live in SlotCardCatalog.h so the
// Slot Manager panel shares them. Bring the names into this TU unqualified
// to keep the existing panel body unchanged.
using pom2::kCardTypes;
using pom2::mouseRomsPresent;
using pom2::mouseAwRomPresent;
using pom2::cffaRomPresent;

void MainWindow::renderSlotConfigPanel()
{
    if (!uiState_->showSlotConfigPanel) return;

    const auto& effectivePlan = slotCoordinator_->effectivePlan();
    auto& draft = slotCoordinator_->draft();
    pom2::SlotConfigurationCoordinator::LiveSnapshot liveSlots;
    {
        auto state = controller->lockState();
        liveSlots = slotCoordinator_->captureLive(state.memory().slotBus());
    }

    // 880 px was sized for two columns; one column needs about half that.
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Slot Configuration", &uiState_->showSlotConfigPanel)) {
        ImGui::End();
        return;
    }

    // This window is ONE interaction model: staged. It used to carry the media
    // column too, and the two models sat side by side with nothing but a
    // banner to tell them apart — Apply / Revert at the bottom of the left
    // child read as governing the whole window, so mounting a disk on the
    // right and hitting Revert on the left looked like it should undo the
    // mount. The media half now lives in its own window (Devices → Internal
    // Disks & Media), which is what makes Apply / Revert unambiguous.
    ImGui::TextWrapped(
        "Assign a card to each expansion slot. Changes are staged until you "
        "Apply — that restarts the emulator. Mounting media is a separate "
        "window: Devices \xe2\x86\x92 Internal Disks & Media.");
    ImGui::Spacing();

    const auto& profileCfg = pom2::profileConfig(activeProfile);

    ImGui::BeginChild("##slotassign", ImVec2(0.0f, 0.0f),
                      ImGuiChildFlags_Borders);
    {
        ImGui::SeparatorText("Expansion slots");

        // Slot number leads, control fills the rest of the row. ImGui's native
        // LabelText / BeginCombo put their label on the RIGHT, so the panel
        // read "(empty) v  Slot 1" — the number, which is exactly what the eye
        // scans down, trailed its own control. Gutter measured off the widest
        // label so it survives the UI zoom.
        const float slotGutter = ImGui::CalcTextSize("AUX slot").x +
                                 ImGui::GetStyle().ItemSpacing.x * 2.0f;
        auto slotLabel = [slotGutter](const char* text) {
            ImGui::TextUnformatted(text);
            ImGui::SameLine(slotGutter);
            ImGui::SetNextItemWidth(-FLT_MIN);
        };

        const bool mouseAvailable    = mouseRomsPresent();
        const bool mouseAwAvailable  = mouseAwRomPresent();
        const bool cffaAvailable     = cffaRomPresent();

        // AUX slot (IIe-class only): built-in 80-column card at $C300 — shown
        // greyed as a non-editable row.
        if (profileCfg.iieMode) {
            ImGui::BeginDisabled(true);
            slotLabel("AUX slot");
            ImGui::TextUnformatted("Extended 80-Column Card (built-in, $C300 firmware)");
            ImGui::EndDisabled();
            ImGui::Spacing();
        }

        // "diskii" is multi-instance — never flagged as a duplicate.
        // Built-in slots forced by the profile are also exempt: e.g. //c
        // ships TWO SSC-compatible serial ports at sl1+sl2 (printer +
        // modem), both forced by cfgAppleIIc, and the user picker must
        // not light them up red. Same logic as plugSlotsFromSettings'
        // uniqueness check.
        auto isDuplicate = [&](int slot) -> bool {
            if (draft[slot].empty())                    return false;
            if (draft[slot] == "diskii")                return false;
            if (profileCfg.builtInSlots[slot].has_value()) return false;
            for (int s = 1; s <= 7; ++s) {
                if (s == slot) continue;
                if (profileCfg.builtInSlots[s].has_value()) continue;
                if (draft[s] == draft[slot])            return true;
            }
            return false;
        };

        // Does the profile already ship a Le Chat Mauve as an on-board fixture
        // (//c PAL = "Adaptateur IIc")? If so, the rear-connector adapter is
        // taken — don't let the no-physical-slots rows offer a second one.
        bool builtinRgb = false;
        for (int s = 1; s <= 7; ++s)
            if (profileCfg.builtInSlots[s].has_value() &&
                profileCfg.builtInSlots[s]->cardKey == "chatmauve")
                builtinRgb = true;

        bool anyDuplicate = false;
        for (int s = 1; s <= 7; ++s) {
            char label[32];
            std::snprintf(label, sizeof(label), "Slot %d", s);

            // Profile built-in slot → read-only, greyed, with a badge. The
            // card key is forced regardless of user edits; sync the draft so
            // an Apply persists the locked value over a stale saved key.
            if (profileCfg.builtInSlots[s].has_value()) {
                const auto& bis = *profileCfg.builtInSlots[s];
                draft[s] = bis.cardKey;
                const char* cardName = bis.cardKey.c_str();
                for (const auto& ct : kCardTypes) {
                    if (ct.key == bis.cardKey) { cardName = ct.label; break; }
                }
                char preview[96];
                std::snprintf(preview, sizeof(preview),
                              "%s — %s", cardName, bis.label.c_str());
                ImGui::BeginDisabled(true);
                slotLabel(label);
                ImGui::TextUnformatted(preview);
                ImGui::EndDisabled();
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Built into %s — cannot be changed.",
                                      std::string(profileCfg.displayName).c_str());
                continue;
            }

            // Profile has no physical expansion BUS (//c / //c+) — peripheral
            // cards can't be plugged. The ONE exception is the Le Chat Mauve
            // RGB card: on a //c it's the "Adaptateur IIc" that goes on the
            // rear DB-15 video-expansion connector (which the //c does have).
            // So offer a {empty, Le Chat Mauve} toggle on each virtual slot
            // and nothing else; the duplicate check keeps it to one adapter.
            if (profileCfg.noPhysicalSlots) {
                if (draft[s] != "chatmauve") draft[s] = "";
                // RGB adapter already on-board (//c PAL) → this slot is just
                // a non-existent connector; grey it out like the others.
                if (builtinRgb) {
                    draft[s] = "";
                    ImGui::BeginDisabled(true);
                    slotLabel(label);
                    ImGui::Text("(no physical slot on %s)",
                                std::string(profileCfg.displayName).c_str());
                    ImGui::EndDisabled();
                    continue;
                }
                const char* preview = (draft[s] == "chatmauve")
                    ? "Le Chat Mauve RGB (rear connector)" : "(empty)";
                slotLabel(label);
                char comboId[24];
                std::snprintf(comboId, sizeof(comboId), "##slotcombo%d", s);
                if (ImGui::BeginCombo(comboId, preview)) {
                    if (ImGui::Selectable("(empty)", draft[s].empty()))
                        draft[s] = "";
                    if (ImGui::Selectable("Le Chat Mauve RGB (rear connector)",
                                          draft[s] == "chatmauve"))
                        draft[s] = "chatmauve";
                    ImGui::EndCombo();
                }
                if (draft[s] == "chatmauve" && isDuplicate(s)) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                                       "(one adapter only)");
                    anyDuplicate = true;
                }
                continue;
            }

            const bool dup = isDuplicate(s);
            if (dup) anyDuplicate = true;

            const char* preview = "(empty)";
            for (const auto& ct : kCardTypes) {
                if (ct.key == draft[s]) { preview = ct.label; break; }
            }

            // A staged row is marked where the user is looking — on the row
            // itself — not only by the button at the bottom of the column.
            const bool staged = (draft[s] != effectivePlan[s]);
            const bool liveDiffers = (liveSlots.keys[s] != effectivePlan[s]);
            if (!staged && liveDiffers) {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.25f, 1.0f),
                                   ICON_FA_PLUG_CIRCLE_EXCLAMATION);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Configured: %s\nActually plugged: %s\n"
                        "The plan is preserved; this can be a missing ROM, "
                        "a fallback implementation or a session-only card.",
                        pom2::cardLabelForKey(effectivePlan[s]),
                        liveSlots.plugged(s) ? liveSlots.names[s].c_str()
                                             : "(empty)");
                }
                ImGui::SameLine(0.0f, 0.0f);
            } else if (staged) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(pom2::palette().accent));
                ImGui::TextUnformatted(ICON_FA_CIRCLE_DOT);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip(
                        "Staged. Effective configuration: %s\n"
                        "Actually plugged: %s",
                        pom2::cardLabelForKey(effectivePlan[s]),
                        liveSlots.plugged(s) ? liveSlots.names[s].c_str()
                                             : "(empty)");
                }
                ImGui::SameLine(0.0f, 0.0f);
            }
            slotLabel(label);
            char comboId[24];
            std::snprintf(comboId, sizeof(comboId), "##slotcombo%d", s);
            if (dup) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
            if (ImGui::BeginCombo(comboId, preview)) {
                for (const auto& ct : kCardTypes) {
                    const bool selected = (ct.key == draft[s]);
                    const bool disabled =
                        ((std::string(ct.key) == "mouse")   && !mouseAvailable) ||
                        ((std::string(ct.key) == "mouseaw") && !mouseAwAvailable) ||
                        ((std::string(ct.key) == "cffa")    && !cffaAvailable);
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

            // Slot 3 on a //e-class machine is where the built-in 80-column
            // firmware keeps OURCH/OURCV — the screen holes at $x78+3 are
            // its scratchpad, not the card's. Printer firmware stores its
            // column and line counters there, so a Grappler+/Printer card
            // in slot 3 reads the cursor position back as its line width
            // and wraps after every character (real hardware does exactly
            // the same — the Grappler+ manual says slot 1). Everything
            // else about the card works, so warn instead of forbidding.
            if (s == 3 && profileCfg.iieMode &&
                (draft[s] == "grappler" || draft[s] == "printer")) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                                   "(80-col firmware owns slot 3 — use 1)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "On a //e the internal 80-column firmware uses the "
                        "slot-3 screen holes ($0478+3, $057B, $05FB…) for "
                        "its own cursor state.\nA printer card in that slot "
                        "shares them and prints one character per line.\n"
                        "Move it to slot 1 (or 2/4/5/7) — same as on real "
                        "hardware.");
            }

            // Slot 3 on a //e is not merely awkward, it is DEAD for almost
            // every card: with SLOTC3ROM off (the reset default) the
            // motherboard owns $C300-$C3FF outright and slot 3's I/O SELECT
            // never asserts. Any card that decodes anything in its $Cs00
            // page is unreachable there — which is most of them, and not
            // only the ones with firmware: a Mockingboard addresses its
            // VIAs through that window too (see MockingboardCard::
            // slotRomRead), so it is as invisible as a mouse.
            //
            // Real hardware behaves the same way, which is why Apple sold
            // the mouse for slot 4 and why the //e manual tells you to leave
            // slot 3 to the 80-column card. Warned, not forbidden: a user
            // who knows to flip SLOTC3ROM can still have it.
            if (s == 3 && profileCfg.iieMode && !draft[s].empty() &&
                draft[s] != "grappler" && draft[s] != "printer") {
                const bool isMouse =
                    (draft[s] == "mouse" || draft[s] == "mouseaw");
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                                   isMouse
                                       ? "(invisible in slot 3 — use 4)"
                                       : "(slot 3 $C300 window is dead)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        isMouse
                        ? "On a //e the internal 80-column firmware owns "
                          "$C300-$C3FF, so a card there has NO $Cs00 page the "
                          "guest can reach.\nSoftware finds the mouse by "
                          "scanning slots for the Apple signature ($Cn05=$38, "
                          "$Cn07=$18, $Cn0B=$01, $Cn0C=$20) — at $C300 it "
                          "reads the 80-column firmware instead and decides "
                          "there is no mouse.\nA2DeskTop, MousePaint and "
                          "MultiScribe then run keyboard-only.\nMove it to "
                          "slot 4 (Apple's own slot for it), or 5/7 — same as "
                          "on real hardware."
                        : "On a //e the internal 80-column firmware owns "
                          "$C300-$C3FF, so slot 3's I/O SELECT never asserts "
                          "and NOTHING in the card's $C300 page is "
                          "reachable.\nThat kills any card that needs it — "
                          "firmware the guest scans for, and registers too: a "
                          "Mockingboard addresses its VIAs through that "
                          "window, so it goes silent there.\nA card that "
                          "only uses its $C0nX soft switches still works.\n"
                          "On real hardware slot 3 belongs to the 80-column "
                          "card.");
            }
        }

        ImGui::Spacing();
        ImGui::Separator();

        if (mouseAvailable) {
            ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                               "Mouse ROMs found.");
        } else {
            ImGui::TextColored(ImVec4(0.95f, 0.6f, 0.4f, 1.0f),
                               "Mouse ROMs missing — Mouse Interface disabled. "
                               "Add roms/mouse_341-0270-c.bin + "
                               "roms/mouse_341-0269.bin.");
        }
        if (!mouseRomStatus.empty())
            ImGui::TextWrapped("Mouse: %s", mouseRomStatus.c_str());

        ImGui::Spacing();
        if (anyDuplicate) {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                               "One card type per slot — fix duplicates.");
        }

        // How many user-editable slots differ from what is actually plugged.
        // Built-in slots are force-fed into the draft by the rows above, so
        // they can never count as pending.
        int pending = 0;
        for (int s = 1; s <= 7; ++s) {
            if (profileCfg.builtInSlots[s].has_value()) continue;
            if (draft[s] != effectivePlan[s]) ++pending;
        }

        if (pending > 0) {
            ImGui::TextColored(
                ImGui::ColorConvertU32ToFloat4(pom2::palette().accent),
                ICON_FA_CIRCLE_DOT " %d staged change%s — not applied yet",
                pending, pending == 1 ? "" : "s");
        } else {
            ImGui::TextDisabled("No staged changes.");
        }

        // Apply is disabled with nothing staged: a button that restarts the
        // emulator should never be a no-op the user can hit by reflex.
        ImGui::BeginDisabled(anyDuplicate || pending == 0);
        char applyLabel[64];
        std::snprintf(applyLabel, sizeof(applyLabel),
                      pending > 0 ? "Apply %d change%s (cold-boots the machine)"
                                  : "Apply (cold-boots the machine)",
                      pending, pending == 1 ? "" : "s");
        if (ImGui::Button(applyLabel)) {
            // Persist ONLY user-editable slots. The panel force-feeds the
            // draft with the profile's built-in cards and force-empties the
            // non-existent connectors on a noPhysicalSlots machine (see the
            // rows above), so persisting all seven here clobbered the
            // user's saved //e-era slot_N_card keys whenever Apply was
            // clicked on a //c-class profile. Same guard as the
            // ~MainWindow shutdown persist path.
            std::array<std::string, 8> previous{};
            std::array<bool, 8> changed{};
            for (int s = 1; s <= 7; ++s) {
                const std::string key = "slot_" + std::to_string(s) + "_card";
                if (!pom2::slotKeyIsUserChoice(profileCfg, s, draft[s],
                                               settings->getString(key, "")))
                    continue;
                previous[s] = settings->getString(key, "");
                changed[s] = true;
                settings->setString(key, draft[s]);
            }
            if (!settings->save()) {
                for (int s = 1; s <= 7; ++s) {
                    if (changed[s]) settings->setString(
                        "slot_" + std::to_string(s) + "_card", previous[s]);
                }
                uiState_->tapeStatusMessage = "Slot changes not applied — settings could not be saved.";
                uiState_->tapeStatusUntil = uiState_->lastFrameTime + 8.0;
                pom2::log().warn("Slots", uiState_->tapeStatusMessage);
            } else if (!restartEmulationFromSettings()) {
                // The live machine was deliberately left intact. Restore the
                // persisted mapping too, otherwise the refused draft would be
                // applied silently on the next launch.
                for (int s = 1; s <= 7; ++s) {
                    if (changed[s]) settings->setString(
                        "slot_" + std::to_string(s) + "_card", previous[s]);
                }
                if (!settings->save())
                    pom2::log().error("Slots",
                        "Could not persist the previous slot mapping after a refused rebuild.");
            } else {
                // restartEmulationFromSettings also captured live media paths
                // after the first save; make those refreshed values durable.
                if (!settings->save()) {
                    uiState_->tapeStatusMessage =
                        "Slots applied, but updated settings could not be saved.";
                    uiState_->tapeStatusUntil = uiState_->lastFrameTime + 8.0;
                    pom2::log().warn("Slots", uiState_->tapeStatusMessage);
                }
                slotCoordinator_->resetDraft();
            }
        }
        ImGui::EndDisabled();
        if (pending > 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Cold-boots the emulated machine with the new cards —\n"
                "RAM is wiped, exactly as if you had powered it off,\n"
                "swapped the cards and powered it back on. Anything\n"
                "running or loaded in memory is gone.\n"
                "Mounted media is preserved where the card still exists.\n"
                "Only affects the slot list above — anything mounted from\n"
                "Internal Disks & Media has already taken effect.");
        ImGui::SameLine();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::Button("Revert")) slotCoordinator_->resetDraft();
        ImGui::EndDisabled();
        if (pending > 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip("Discard the %d staged slot change%s.\n"
                              "Does not touch mounted media.",
                              pending, pending == 1 ? "" : "s");
    }
    ImGui::EndChild();

    ImGui::End();
}

// ─── Internal Disks & Media ─────────────────────────────────────────────
// Split out of Slot Configuration on 2026-07-28. Everything here is
// IMMEDIATE — Mount / Insert / Eject act on the running machine — which is
// the opposite of the staged model next door, and the reason the two no
// longer share a window.
void MainWindow::renderMediaPanel()
{
    if (!uiState_->showMediaPanel) return;

    pom2::SlotConfigurationCoordinator::LiveSnapshot liveSlots;
    {
        auto state = controller->lockState();
        liveSlots = slotCoordinator_->captureLive(state.memory().slotBus());
    }

    ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Internal Disks & Media", &uiState_->showMediaPanel)) {
        ImGui::End();
        return;
    }

    const auto& profileCfg = pom2::profileConfig(activeProfile);

    ImGui::BeginChild("##slotmedia", ImVec2(0, 0), ImGuiChildFlags_Borders);
    {
        ImGui::SeparatorText("Internal disks & mountable ports");
        ImGui::TextDisabled("Mount / Insert / Eject take effect immediately.");
        ImGui::TextDisabled("Which card sits in which slot: Machine \xe2\x86\x92 "
                            "Slot Configuration.");
        ImGui::Spacing();

        // Shared media status LED (grey/green/yellow/red). Kept as a local
        // alias so the existing per-row call sites read unchanged.
        auto dot = [](bool loaded, bool wp) { pom2::statusLed(loaded, wp); };

        // Persistent InputText buffers, keyed [slot][bay/drive]. Primed once
        // from the live path; re-primed (to the new live value) after eject.
        static std::array<std::array<std::array<char, 512>, 2>, 8> mBuf{};
        static std::array<std::array<bool, 2>, 8> mPrimed{};
        static std::array<std::array<std::array<char, 512>, 2>, 8> dBuf{};
        static std::array<std::array<bool, 2>, 8> dPrimed{};

        bool any = false;
        // Unlocked on purpose, and one of the few places `memory()` is the
        // right accessor. What is read here is the bus *topology* (which slot
        // holds which card), and that is UI-thread-confined: every writer —
        // plugSlotsFromSettings, applyProfile, the slot-config rebuild — runs
        // on this thread. The worker only ever reads it, from memRead
        // dispatch. Taking `lockState()` for the reference would protect
        // nothing (it is released before the loop below uses `bus`) while
        // reading as though it did. Per-card *state* is a different matter,
        // and each bay snapshot below does take the lock.
        SlotBus& bus = controller->memory().slotBus();

        for (int s = 1; s <= 7; ++s) {
            SlotPeripheral* p = bus.peripheral(s);
            if (!p) continue;
            const bool builtIn = profileCfg.builtInSlots[s].has_value();

            // ── Cards with mountable bays (SmartPort / CFFA / HDV) ────────
            if (auto* media = dynamic_cast<pom2::MountableMediaCard*>(p)) {
                any = true;
                ImGui::PushID(2000 + s);
                ImGui::Text("Slot %d — %s%s", s,
                            liveSlots.names[s].c_str(),
                            builtIn ? " (built-in)" : "");

                int nb = media->bayCount();
                if (nb > 2) nb = 2;
                bool bootable = false;
                for (int b = 0; b < nb; ++b) {
                    // Snapshot the bay state under the lock — the worker
                    // mutates it during block I/O (loaded/dirty flags,
                    // lastError strings). Same snapshot-under-lock rule as
                    // the Disk II / HDV panels; the lock is NOT held across
                    // the rendering below (the Mount/Eject buttons take it
                    // themselves).
                    pom2::MediaBayInfo info;
                    {
                        std::lock_guard<std::mutex> lk(controller->stateMutex());
                        info = media->bayInfo(b);
                    }
                    ImGui::PushID(b);
                    ImGui::Indent();

                    dot(info.loaded, info.writeProtected);
                    if (info.supportsTypeSelect) ImGui::Text("Unit %d", b);
                    else                         ImGui::TextUnformatted("Image");
                    if (info.loaded) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s, %u blocks%s)",
                            info.kindLabel.empty() ? "media" : info.kindLabel.c_str(),
                            info.blockCount, info.writeProtected ? ", WP" : "");
                    } else if (!info.kindLabel.empty()) {
                        ImGui::SameLine();
                        ImGui::TextDisabled("(%s)", info.kindLabel.c_str());
                    }

                    // Type selector (SmartPort units only).
                    if (info.supportsTypeSelect) {
                        const auto opts = media->bayTypeOptions(b);
                        const char* curLabel = "(empty)";
                        for (const auto& o : opts)
                            if (o.first == info.typeKey) { curLabel = o.second.c_str(); break; }
                        ImGui::SetNextItemWidth(150);
                        if (ImGui::BeginCombo("Type", curLabel)) {
                            for (const auto& o : opts) {
                                const bool sel = (o.first == info.typeKey);
                                if (ImGui::Selectable(o.second.c_str(), sel) &&
                                    o.first != info.typeKey) {
                                    const auto command =
                                        storageCoordinator_->setMediaBayType(
                                            *controller, *settings, s, b,
                                            o.first);
                                    if (command.ok) mPrimed[s][b] = false;
                                }
                                if (sel) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }
                    }

                    const bool typeAllows =
                        !info.supportsTypeSelect || !info.typeKey.empty();

                    char* buf = mBuf[s][b].data();
                    if (!mPrimed[s][b]) {
                        std::snprintf(buf, mBuf[s][b].size(), "%s", info.path.c_str());
                        mPrimed[s][b] = true;
                    }
                    ImGui::SetNextItemWidth(300);
                    ImGui::InputText("##path", buf, mBuf[s][b].size());
                    ImGui::SameLine();
                    ImGui::BeginDisabled(buf[0] == '\0' || !typeAllows);
                    if (ImGui::Button("Mount")) {
                        const auto command =
                            storageCoordinator_->mountMediaBay(
                                *controller, *settings, s, b, buf);
                        uiState_->tapeStatusMessage = command.ok
                            ? ("Slot " + std::to_string(s) + ": mounted " + buf)
                            : ("Slot " + std::to_string(s) +
                               ": mount failed: " + command.error);
                        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!info.loaded);
                    if (ImGui::Button("Eject")) {
                        const auto command =
                            storageCoordinator_->ejectMediaBay(
                                *controller, *settings, s, b);
                        if (command.ok) mPrimed[s][b] = false;
                        uiState_->tapeStatusMessage = "Slot " + std::to_string(s) +
                            (command.ok ? ": ejected"
                                        : ": eject failed: " + command.error);
                        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();

                    if (info.supportsWriteBack) {
                        bool wb = info.writeBackEnabled;
                        ImGui::BeginDisabled(!typeAllows);
                        if (ImGui::Checkbox("Write-back (save on eject)", &wb)) {
                            (void)storageCoordinator_->setMediaBayWriteBack(
                                *controller, *settings, s, b, wb);
                        }
                        ImGui::EndDisabled();
                    }

                    if (!info.lastError.empty())
                        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.5f, 1.0f),
                                           "Error: %s", info.lastError.c_str());

                    if (b == 0 && info.loaded) bootable = true;
                    ImGui::Unindent();
                    ImGui::PopID();
                }

                ImGui::BeginDisabled(!bootable);
                if (ImGui::SmallButton("Boot slot")) {
                    controller->bootFromSlot(s);
                    uiState_->tapeStatusMessage = "Booting slot " + std::to_string(s);
                    uiState_->tapeStatusUntil = uiState_->lastFrameTime + 3.0;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::Separator();
            }
            // ── Internal Disk II drives (5.25") ───────────────────────────
            else if (auto* d2 = dynamic_cast<DiskIICard*>(p)) {
                any = true;
                ImGui::PushID(3000 + s);
                ImGui::Text("Slot %d — %s%s", s,
                            liveSlots.names[s].c_str(),
                            builtIn ? " (built-in)" : "");

                bool bootable = false;
                for (int drv = 0; drv < DiskIICard::kDriveCount; ++drv) {
                    // Snapshot under the lock (worker mutates load state /
                    // path during inserts from other panels); not held
                    // across rendering — the buttons lock themselves.
                    bool loaded;
                    std::string path;
                    {
                        std::lock_guard<std::mutex> lk(controller->stateMutex());
                        loaded = d2->isDiskLoaded(drv);
                        if (!dPrimed[s][drv]) path = d2->getDiskPath(drv);
                    }
                    if (drv == 0 && loaded) bootable = true;
                    ImGui::PushID(drv);
                    ImGui::Indent();
                    dot(loaded, false);
                    ImGui::Text("Drive %d", drv + 1);

                    char* buf = dBuf[s][drv].data();
                    if (!dPrimed[s][drv]) {
                        std::snprintf(buf, dBuf[s][drv].size(), "%s",
                                      path.c_str());
                        dPrimed[s][drv] = true;
                    }
                    ImGui::SetNextItemWidth(300);
                    ImGui::InputText("##d2path", buf, dBuf[s][drv].size());
                    ImGui::SameLine();
                    ImGui::BeginDisabled(buf[0] == '\0');
                    if (ImGui::Button("Insert")) {
                        const auto command = storageCoordinator_->mountDiskII(
                            *controller, *settings, s, drv, buf, true);
                        uiState_->tapeStatusMessage = command.ok
                            ? ("Slot " + std::to_string(s) + " drive " +
                               std::to_string(drv + 1) + ": inserted")
                            : ("Slot " + std::to_string(s) + ": insert failed: " +
                               command.error);
                        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!loaded);
                    if (ImGui::Button("Eject")) {
                        const auto command = storageCoordinator_->ejectDiskII(
                            *controller, *settings, s, drv);
                        if (command.ok) dPrimed[s][drv] = false;
                        uiState_->tapeStatusMessage = "Slot " + std::to_string(s) +
                            " drive " + std::to_string(drv + 1) +
                            (command.ok ? ": ejected" : ": eject failed: " +
                                          command.error);
                        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::Unindent();
                    ImGui::PopID();
                }

                ImGui::BeginDisabled(!bootable);
                if (ImGui::SmallButton("Boot slot")) {
                    controller->bootFromSlot(s);
                    uiState_->tapeStatusMessage = "Booting slot " + std::to_string(s);
                    uiState_->tapeStatusUntil = uiState_->lastFrameTime + 3.0;
                }
                ImGui::EndDisabled();
                ImGui::PopID();
                ImGui::Separator();
            }
        }

        if (!any)
            ImGui::TextDisabled("No storage cards plugged.");
    }
    ImGui::EndChild();

    ImGui::End();
}

// ─── Emulation restart ──────────────────────────────────────────────────

std::string MainWindow::firstExistingPath(const std::vector<std::string>& candidates)
{
    // pom2::findFirstResource probes each candidate against CWD, the
    // build/-relative `../` `../../` roots (dev), and the executable-
    // relative / FHS-install roots (portable bundle, AppImage, /usr/bin).
    // See ResourcePaths.h.
    return pom2::findFirstResource(candidates);
}

M6502::CpuMode MainWindow::resolveCpuMode(M6502::CpuMode profileDefault) const
{
    const std::string override = settings->getString("cpu_mode_override", "auto");
    // A 65C02 is a strict superset of the NMOS 6502, so forcing CMOS is
    // always physically plausible (it was a real socket-upgrade on II/II+).
    if (override == "65c02") return M6502::CpuMode::CMOS;
    // Forcing NMOS only makes sense on a machine that actually shipped an
    // NMOS 6502 (II / II+ / //e-unenhanced → profileDefault == NMOS). The
    // //c, //c+, enhanced //e and the PAL variants have a 65C02 SOLDERED in
    // — they cannot run NMOS, and their ROMs use 65C02-only opcodes (e.g.
    // LDA (zp) = $B2) that DECODE AS KIL on an NMOS core and freeze the CPU.
    // That was the "//c hangs / POM2 freezes when I switch to it via the
    // menu" bug: a sticky `cpu_mode_override=nmos` (set once on a II+) was
    // dragged onto the //c. So an NMOS override is honoured only where the
    // machine supports it; on a CMOS-only profile the profile default wins.
    if (override == "nmos" && profileDefault == M6502::CpuMode::NMOS)
        return M6502::CpuMode::NMOS;
    return profileDefault;     // "auto", or NMOS-override on a CMOS-only machine
}

float MainWindow::floppyMotorPitchForProfile(pom2::SystemProfile p)
{
    switch (p) {
        case pom2::SystemProfile::AppleIIc:
        case pom2::SystemProfile::AppleIIcPlus:
        case pom2::SystemProfile::AppleIIcPAL:
            return 1.4f;       // Sony internal drive ≈ 40% faster spin-up
        default:
            return 1.0f;       // original Disk II Shugart — native rate
    }
}

void MainWindow::setGlfwWindow(GLFWwindow* w)
{
    window = w;
    // Catch up the title once the handle is available — the constructor
    // may have resolved a non-default profile before main.cpp could hand
    // us the window, so the initial title from glfwCreateWindow wouldn't
    // reflect the active machine otherwise.
    if (window) {
        const auto& cfg = pom2::profileConfig(activeProfile);
        std::string title = "POM2 " POM2_VERSION_STRING " — ";
        title.append(cfg.displayName);
        glfwSetWindowTitle(window, title.c_str());

        // Reopen at the geometry the last windowed session ended with.
        // Skipped in kiosk (main() already created the window full-screen)
        // and when nothing was ever persisted, in which case main()'s
        // default size stands. A saved position is clamped back onto a
        // monitor so a window saved on a since-disconnected screen can't
        // reopen off-screen.
        if (!uiState_->kiosk && loadWindowGeometryFromSettings()) {
            // Validate against the WHOLE virtual desktop, not just the
            // primary monitor: a monitor to the left of primary has
            // NEGATIVE virtual-screen X and one to the right has X beyond
            // the primary width, so a primary-only clamp dragged every
            // secondary-display window back to the centre of screen 1 on
            // each launch, with no way to make it stick.
            int mc = 0;
            GLFWmonitor** mons = glfwGetMonitors(&mc);
            bool onSomeMonitor = false;
            for (int i = 0; i < mc && !onSomeMonitor; ++i) {
                int mx = 0, my = 0, mw = 0, mh = 0;
                glfwGetMonitorWorkarea(mons[i], &mx, &my, &mw, &mh);
                // "Visible enough to grab": the title bar's left corner
                // must sit inside this monitor's work area.
                if (uiState_->savedWindowX >= mx - 32 && uiState_->savedWindowX <= mx + mw - 64 &&
                    uiState_->savedWindowY >= my - 32 && uiState_->savedWindowY <= my + mh - 64)
                    onSomeMonitor = true;
            }
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (vm) {
                if (uiState_->savedWindowWidth > vm->width)  uiState_->savedWindowWidth = vm->width;
                if (uiState_->savedWindowHeight > vm->height) uiState_->savedWindowHeight = vm->height;
            }
            if (!onSomeMonitor && vm) {
                // Saved on a since-disconnected screen — recentre on
                // primary rather than reopening off-screen.
                uiState_->savedWindowX = (vm->width  - uiState_->savedWindowWidth) / 2;
                uiState_->savedWindowY = (vm->height - uiState_->savedWindowHeight) / 2;
            }
            glfwSetWindowSize(window, uiState_->savedWindowWidth, uiState_->savedWindowHeight);
            glfwSetWindowPos (window, uiState_->savedWindowX, uiState_->savedWindowY);
            if (uiState_->savedWindowMaximized) glfwMaximizeWindow(window);
        }
    }
}

void MainWindow::applyProfile(pom2::SystemProfile p)
{
    const auto& cfg = pom2::profileConfig(p);
    pom2::log().info("Profile",
        std::string("Switching to ") + std::string(cfg.displayName));

    const bool wasRunning =
        controller->getMode() == EmulationController::Mode::Running;
    controller->stop();
    std::string flushErr;
    if (!flushSlotMedia(flushErr)) {
        uiState_->tapeStatusMessage = "Profile switch refused — save failed: " + flushErr;
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 8.0;
        pom2::log().warn("Profile", uiState_->tapeStatusMessage);
        if (wasRunning) controller->start();
        return;
    }

    // Durability is established: start the shared topology transaction. It
    // invalidates rewind state and session-local auto-provision markers now,
    // before any card is destroyed.
    slotRebuildCoordinator_->prepareAfterFlush();

    // 0. Commit the active profile NOW — BEFORE step 7's plugSlotsFromSettings(),
    //    which reads `activeProfile` to apply the profile's built-in locked slots
    //    (//c / //c+ on-board SSC / Mouse / SmartPort / Disk II). Setting it only
    //    at step 12 meant the re-plug used the PREVIOUS profile's built-ins:
    //    switching INTO //c/c+ never forced its on-board cards (no boot disk
    //    controller — also at startup, where the ctor calls applyProfile(saved)),
    //    and switching AWAY leaked //c built-ins into a clean II+/IIe. Everything
    //    between here and step 7 keys off the local `cfg`/`p`, not the member.
    activeProfile = p;

    // 1. The worker was stopped before the media flush above, so card
    //    destructors cannot race a CPU step or worker idle-loop probe.
    // The transaction has already dropped the rewind ring: it describes the
    // previous machine and cannot be restored onto the replacement topology.

    // 2. Snapshot currently-mounted media by value so a user can test the
    //    same disks under another machine profile. StorageCoordinator owns
    //    the Disk II/HDV/CFFA policies and no card alias escapes this lock.
    pom2::StorageCoordinator::RebuildSnapshot savedMedia;
    {
        auto st = controller->lockState();
        savedMedia = storageCoordinator_->captureRebuildSnapshot(
            st.memory().slotBus());
    }

    // 3. Tear down all slot cards under the state mutex. The shared
    //    coordinator gates AI, detaches audio/UI consumers, clears SlotBus,
    //    and retires network/display state in one fixed order.
    {
        auto st = controller->lockState();
        slotRebuildCoordinator_->beginLocked(st);

        // 4. Cold-reset memory: wipe user RAM, aux RAM (if IIe), LC banks,
        //    soft switches. setIIEMode FIRST, for two reasons:
        //    (a) clearRam() wipes aux / aux-LC / RamWorks ONLY when iieMode is
        //        set — so switching INTO a IIe-class profile must flip the mode
        //        before the wipe, or the new machine inherits the previous
        //        session's aux RAM instead of a clean 00/FF cold-boot pattern
        //        (round 9 #6);
        //    (b) loadAppleIIRom (step 5) populates internalIORom only when
        //        iieMode is true for a 16/32 KB dump, so the mode must be set
        //        before the load too.
        st.memory().setIIEMode(cfg.iieMode);
        st.memory().clearRam();
        st.memory().resetSoftSwitches();

        // RamWorks III — Applied Engineering aux-slot RAM expansion.
        // Plugs into the IIe aux slot, present on BOTH the 1983 Unenhanced
        // and 1985 Enhanced //e; only //c and //c+ lack it (their aux RAM is
        // on the motherboard, no expansion bus). Gate on either //e variant
        // so $C073 writes on //c stay in the paddle-reset-only path. Tiers:
        // 1 (stock 64K), 4 (256K), 8 (512K), 16 (1M), 48 (3M), 128 (8M).
        // Default 1 = no RamWorks. The setIIEMode(false) branch already
        // cleared backing storage.
        if (p == pom2::SystemProfile::AppleIIe ||
            p == pom2::SystemProfile::AppleIIeUnenhanced ||
            p == pom2::SystemProfile::AppleIIePAL) {
            const int banks = settings->getInt("ramworks_banks", 1);
            st.memory().setRamWorksBanks(
                static_cast<uint32_t>(banks > 0 ? banks : 1));
        } else if (cfg.iieMode) {
            // //c / //c+ — force RamWorks off (might be left over from a
            // prior IIe-profile session). setRamWorksBanks(1) releases
            // the backing.
            st.memory().setRamWorksBanks(1);
        }
    }

    // 5-7 run under stateMutex: the CPU worker is stopped, but the AI
    // control server stays live (detach() nulls only its card pointers,
    // not ctrl_) and its handlers take this same mutex around
    // softReset()/memory reads — without the lock a /reset landing here
    // raced the ROM array rewrite and the SlotBus unique_ptr swaps
    // (torn pointer read / fetch from a half-written ROM). Handlers now
    // simply block until the rebuild is coherent. hardReset (step 11)
    // stays OUTSIDE: it re-acquires stateMtx internally.
    std::string newRomPath;   // read by the "Profile: Active" log below
    {
    auto st = controller->lockState();

    // 5. Resolve and load the new main ROM.
    //    //c / //c+ 32 KB dumps are two firmware banks (bank 0 lower,
    //    bank 1 upper) where the //e 32 KB layout uses "char ROM lower,
    //    firmware upper" — same file size, opposite slicing. Tell the
    //    loader which way to slice based on the active profile.
    const bool pickLowerHalf =
        (p == pom2::SystemProfile::AppleIIc ||
         p == pom2::SystemProfile::AppleIIcPlus ||
         p == pom2::SystemProfile::AppleIIcPAL);
    newRomPath = firstExistingPath(cfg.romProbeOrder);
    if (!newRomPath.empty()
        && st.memory().loadAppleIIRom(newRomPath.c_str(), pickLowerHalf)) {
        romPath  = newRomPath;
        romStatus = std::string(cfg.iieMode ? "IIe/IIc: " : "loaded: ") + newRomPath;
        romLoaded_ = true;
        // ROM identity check (Theme 9, gaps B-4-1 / B-4-2): the generic
        // "apple2.rom" fallback was originally added for legacy POM2
        // installs but it silently misroutes — a user running the II
        // Original profile against an apple2p Applesoft dump gets the
        // wrong BASIC dialect. Warn so they at least see the mismatch
        // in the log.
        if (newRomPath.find("apple2.rom") != std::string::npos &&
            cfg.romProbeOrder.front() != newRomPath) {
            pom2::log().warn("Profile",
                std::string("Loaded generic fallback ") + newRomPath +
                " for " + std::string(cfg.displayName) +
                " — profile-specific ROM (" + cfg.romProbeOrder.front() +
                ") not found; ROM identity may not match the selected machine");
        }
    } else {
        romStatus = std::string("NO ROM (") + cfg.romProbeOrder.front() +
                    " not found) — $D000-$FFFF stub only";
        romLoaded_ = false;
        pom2::log().warn("Profile", romStatus);
    }

    // 6. Char ROM. The user's toolbar choice (`charRomLocale`) wins over
    //    the profile probe — switching IIe ↔ IIc shouldn't lose a
    //    "Français" selection. Drop to the profile probe only when the
    //    chosen file vanished (deleted between sessions) or the locale
    //    explicitly says ProfileDefault, AND fall back further to the
    //    profile probe order so we never leave Apple2Display with a
    //    stale csbits table from the previous profile.
    std::string newCharPath;
    if (charRomLocale != pom2::CharRomLocale::ProfileDefault) {
        // resolveCharRomPath probes roms/X, ../roms/X, ../../roms/X so
        // the override works whether POM2 is launched from the repo
        // root or from build/.
        newCharPath = pom2::resolveCharRomPath(charRomLocale);
    }
    if (newCharPath.empty()) {
        newCharPath = firstExistingPath(cfg.charRomProbeOrder);
    }
    charRomPath = newCharPath;
    if (!newCharPath.empty()) {
        st.memory().loadCharRom(newCharPath.c_str(),
                                         pom2::charRomBank(charRomLocale));
    }
    if (cfg.iieMode) display->setAuxMemory(st.memory().auxData());
    else             display->setAuxMemory(nullptr);

    // 7. Re-plug slot cards. plugSlotsFromSettings honours user's
    //    persisted slot config; the profile choice doesn't override that
    //    (e.g. a user who put SSC in slot 4 keeps it across profile
    //    switches).
    plugSlotsFromSettings(st);
    restoreSlotMediaFromSettings(st);
    // resolve() inside plugSlotsFromSettings also re-seeds the staged draft
    // from the newly effective profile plan.

    }   // end stateMutex scope over steps 5-7

    // 7b. A profile that ships an on-board Le Chat Mauve (//c PAL = the
    //     Adaptateur IIc machine) defaults its display to ChatMauveRGB — the
    //     whole point of that profile is the RGB output, so a fresh user sees
    //     it without hunting through the View → Hi-res menu. The card was just
    //     plugged above, so the mode is immediately meaningful. The user can
    //     still pick another mode afterwards (it persists until the next load
    //     of this profile). Other profiles leave the display mode untouched.
    {
        bool builtinRgb = false;
        for (int s = 1; s <= 7; ++s)
            if (cfg.builtInSlots[s].has_value() &&
                cfg.builtInSlots[s]->cardKey == "chatmauve")
                builtinRgb = true;
        if (builtinRgb &&
            devicePanelCoordinator_->captureInventory().chatMauvePlugged())
            display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    }

    // 8. Re-mount the live session snapshot. The coordinator resolves every
    //    target from the replacement SlotBus and silently skips card-type or
    //    file changes. Keep this under the same state lock used by AI media
    //    handlers; the CPU is stopped but those HTTP handlers remain live.
    {
        auto st = controller->lockState();
        storageCoordinator_->restoreRebuildSnapshot(
            st.memory().slotBus(), savedMedia);
    }

    // 9. CPU mode (profile default with optional user override).
    bool cpuIsCmos = false;
    {
        auto st = controller->lockState();
        st.cpu().setCpuMode(resolveCpuMode(cfg.defaultCpu));
        // Capture it here rather than re-reading unlocked for the log
        // below, which is outside this scope.
        cpuIsCmos = (st.cpu().getCpuMode() == M6502::CpuMode::CMOS);
    }

    // 10. Default CPU pacing + video standard (NTSC 60 Hz / PAL 50 Hz). The
    //     profile's defaultCyclesPerFrame already carries the per-standard
    //     budget (17045 NTSC / 20313 PAL); setVideoStandard sets the worker's
    //     50/60 Hz pacing and propagates the 262/312-line geometry to Memory.
    controller->setCyclesPerFrame(cfg.defaultCyclesPerFrame);
    controller->setVideoStandard(cfg.videoStandard);
    // Re-seed the disk-turbo restore value: it defaults to the NTSC 17045 at
    // construction, and restoring that onto a PAL (or //c+ 4×) profile after
    // a turbo burst would silently underclock the machine.
    diskSavedCyclesPerFrame = cfg.defaultCyclesPerFrame;

    // 11. Final hard reset — CPU re-fetches PC from the new ROM's reset
    //     vector at $FFFC/$FFFD.
    controller->hardReset();
    controller->start();

    // 12. Persist the profile choice for the next launch. (activeProfile was
    //     already committed in step 0 so plugSlotsFromSettings saw the new one.)
    controller->floppySound525().setMotorPitch(floppyMotorPitchForProfile(p));
    // Kiosk is read-only: `POM2 --kiosk --preset ...` must not clobber the
    // user's saved system_profile (or persist anything else) on the way in.
    if (!uiState_->kiosk) {
        settings->setString("system_profile", std::string(cfg.key));
        settings->save();
    }

    // 13. Reflect the profile in the window title so the user sees which
    //     machine is active without opening the Machine → Profile menu.
    //     Skipped when called from the constructor (window not yet set
    //     by main.cpp's setGlfwWindow).
    if (window) {
        std::string title = "POM2 " POM2_VERSION_STRING " — ";
        title.append(cfg.displayName);
        glfwSetWindowTitle(window, title.c_str());
    }

    pom2::log().info("Profile",
        std::string("Active = ") + std::string(cfg.displayName) +
        ", ROM = " + (newRomPath.empty() ? "<missing>" : newRomPath) +
        ", CPU = " +
        (cpuIsCmos ? "65C02" : "NMOS"));

    // Publish slot endpoints only after the rebuilt machine, remounted media
    // and reset sequence are all coherent.
    {
        auto st = controller->lockState();
        slotRebuildCoordinator_->publishLocked(st);
    }
    aiServer->setProfileLabel(std::string(cfg.displayName));
}

bool MainWindow::restartEmulationFromSettings()
{
    // 0. Snapshot LIVE media BEFORE teardown. Menu Insert/Eject and library
    //    mounts update cards before settings, so Slot-Config must synchronize
    //    this value-only state before plugSlotsFromSettings rebuilds them.
    pom2::StorageCoordinator::RebuildSnapshot savedMedia;
    {
        auto st = controller->lockState();
        savedMedia = storageCoordinator_->captureRebuildSnapshot(
            st.memory().slotBus());
    }
    storageCoordinator_->persistRebuildSettings(*settings, savedMedia);
    // (3.5"/SmartPort media is already saved eagerly on mount.)

    // 1. Stop the worker thread so card destructors don't race against a
    //    running CPU step, then prove every opted-in dirty medium is durable
    //    before destroying any card.
    const auto previousMode = controller->getMode();
    controller->stop();
    std::string flushErr;
    if (!flushSlotMedia(flushErr)) {
        uiState_->tapeStatusMessage = "Slot rebuild refused — save failed: " + flushErr;
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 8.0;
        pom2::log().warn("Slots", uiState_->tapeStatusMessage);
        controller->setMode(previousMode);
        return false;
    }
    // The rebuild is committed only after durability succeeds. The shared
    // transaction now invalidates topology-bound history and session-local
    // auto-provision markers before teardown.
    slotRebuildCoordinator_->prepareAfterFlush();

    // 2. Run the exact same ordered teardown used by profile switches.
    //    StateAccess proves the bus mutation is serialized with AI handlers.
    {
        auto st = controller->lockState();
        slotRebuildCoordinator_->beginLocked(st);
    }

    // 3-4 run under stateMutex — same rationale as applyProfile steps
    // 5-7: the AI control server's handlers still run against
    // controller/memory (detach() nulled only its card pointers), so the
    // SlotBus rebuild + remounts must be atomic w.r.t. their lock.
    {
    auto st = controller->lockState();

    // 3. Re-run plugSlotsFromSettings() with the freshly-saved keys.
    plugSlotsFromSettings(st);
    restoreSlotMediaFromSettings(st);
    // resolve() inside plugSlotsFromSettings also re-seeded the staged draft.

    // 4. StorageCoordinator restored Disk II/HDV/CFFA only after every
    //    replacement card existed, including a Disk II newly added by the
    //    staged plan and therefore absent from the old topology snapshot.

    }   // end stateMutex scope over steps 3-4

    // 5. COLD BOOT + restart worker. `coldBoot()`, not `hardReset()`: the
    //    card set just changed, and hardReset preserves RAM — so everything
    //    the guest had built around the OLD hardware survived into the new
    //    machine. DOS 3.3 stays hooked to a slot whose Disk II is gone,
    //    ProDOS keeps a device table describing cards that no longer exist,
    //    a player keeps poking a Mockingboard that was unplugged, and the
    //    warm `resetSoftSwitchesWarm()` even leaves a II/II+'s display and
    //    Language Card banks as they were. The user asked for different
    //    hardware; on a real machine that means opening the lid and powering
    //    back on, which is exactly `coldBoot`: `clearRam()` with the MAME
    //    00/FF pattern, the FULL `resetSoftSwitches()`, and a hard CPU
    //    reset. It matches what `applyProfile` (step 4 + step 11) has always
    //    done for a profile switch — the same event, one rebuild smaller.
    //
    //    Route through the controller rather than `cpu().hardReset()` +
    //    `slotBus().reset()`: the controller path additionally disarms
    //    `iicSmartPortArmed_` (via `Memory::setIicSmartPortArmed(false)`)
    //    and resets the speaker / IWM / SmartPort hub. Pre-fix: on
    //    //c-class, the $C500 firmware punch stayed armed after
    //    `bootFromSlot(5)`, so the post-Apply reset vector was fetched while
    //    the punch was live → //c F8 autostart re-booted SmartPort instead
    //    of leaving the user at the BASIC prompt the Apply was meant to
    //    give them. `coldBoot()` disarms it too.
    controller->coldBoot();
    controller->start();

    // 6. Publish AI slot endpoints after the rebuilt bus is coherent.
    {
        auto st = controller->lockState();
        slotRebuildCoordinator_->publishLocked(st);
    }

    pom2::log().info("Slots",
                     "Cold-booted with the new slot mapping (RAM wiped).");
    return true;
}

// ─── GUI ↔ kiosk runtime transition ──────────────────────────────────────
//
// Kiosk is NOT a different machine: it is exclusive full-screen + the
// chrome-free render path + "never write settings". The emulated CPU,
// memory and slot cards are untouched, so the switch needs no snapshot
// round-trip — flipping the flag and moving the GLFW window is enough,
// and nothing about the running program is disturbed (a game keeps
// playing across the transition, mid-frame).

void MainWindow::saveWindowGeometryToSettings()
{
    if (uiState_->savedWindowWidth <= 0 || !settings) return;
    settings->setInt ("window_x", uiState_->savedWindowX);
    settings->setInt ("window_y", uiState_->savedWindowY);
    settings->setInt ("window_w", uiState_->savedWindowWidth);
    settings->setInt ("window_h", uiState_->savedWindowHeight);
    settings->setBool("window_maximized", uiState_->savedWindowMaximized);
}

bool MainWindow::loadWindowGeometryFromSettings()
{
    if (!settings) return false;
    const int w = settings->getInt("window_w", 0);
    const int h = settings->getInt("window_h", 0);
    if (w <= 0 || h <= 0) return false;
    uiState_->savedWindowX = settings->getInt("window_x", 0);
    uiState_->savedWindowY = settings->getInt("window_y", 0);
    uiState_->savedWindowWidth = w;
    uiState_->savedWindowHeight = h;
    uiState_->savedWindowMaximized = settings->getBool("window_maximized", false);
    return true;
}

void MainWindow::setKioskMode(bool k)
{
    uiState_->kiosk           = k;
    uiState_->launchedInKiosk = k;
    if (k && settings) settings->setReadOnly(true);
}

void MainWindow::captureWindowGeometryNow()
{
    if (!window || uiState_->kiosk || settingsReadOnly()) return;
    // A MAXIMIZED window reports the maximized rect. Do NOT un-maximize to
    // measure: on X11 glfwRestoreWindow only posts a _NET_WM_STATE message
    // and returns, so the very next query still reads the maximized rect —
    // and we would have un-maximized the user's window for nothing. Record
    // the flag and KEEP whatever non-maximized geometry we already had
    // (from an earlier capture or from settings), so re-maximizing on
    // restore lands correctly and un-maximizing afterwards gives a sane
    // floating size instead of a screen-sized rectangle.
    const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
    uiState_->savedWindowMaximized = maximized;
    if (!maximized) {
        glfwGetWindowPos(window, &uiState_->savedWindowX, &uiState_->savedWindowY);
        glfwGetWindowSize(window, &uiState_->savedWindowWidth, &uiState_->savedWindowHeight);
    }
    saveWindowGeometryToSettings();
}

void MainWindow::setKioskModeRuntime(bool k)
{
    if (k == uiState_->kiosk) return;

    if (k) {
        // Entering kiosk. Persist first: kiosk deliberately never writes
        // state.cfg, so anything the user changed in the GUI session would
        // otherwise be lost if they quit from kiosk. (A session LAUNCHED
        // with --kiosk was read-only from the start and stays that way —
        // see settingsReadOnly().)
        if (window) {
#ifdef __EMSCRIPTEN__
            // The browser build must not touch the window/monitor pair at
            // all: Emscripten's GLFW port defines glfwSetWindowMonitor as
            // `abort('glfwSetWindowMonitor not implemented.')` (upstream
            // src/lib/libglfw.js), and an abort() tears the whole module
            // down — the page reports it as a load/init failure and the
            // machine is gone. So the canvas keeps its size and we take the
            // same path as a host with no usable monitor: chrome-free, and
            // nothing else. Real full-screen inside a page belongs to the
            // browser (F11, or the page's own control), not to us.
            pom2::log().info("Kiosk",
                "browser build — chrome-free; canvas size unchanged");
#else
            // Record the windowed geometry to come back to. A MAXIMIZED
            // window reports its maximized size here, so remember the flag
            // separately and re-maximize on the way out — otherwise the
            // user gets an un-maximized window of the maximized size, which
            // most WMs then reposition somewhere unexpected.
            captureWindowGeometryNow();
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (mon && vm) {
                glfwSetWindowMonitor(window, mon, 0, 0,
                                     vm->width, vm->height, vm->refreshRate);
            } else {
                // No monitor info (headless/odd WM): stay windowed but
                // still enter the chrome-free path — the user asked for it.
                pom2::log().warn("Kiosk",
                    "no primary monitor / video mode — kiosk stays windowed");
            }
#endif
        }
        // Persist AFTER measuring, and BEFORE the flag flips: kiosk never
        // writes state.cfg, so this is the last chance to record both the
        // geometry we just captured and anything the user changed in the
        // GUI session. Without it there was nothing to restore from after a
        // quit-from-kiosk, and a --kiosk launch toggling to the GUI got a
        // hard-coded default size instead of the user's real window.
        if (!settingsReadOnly()) {
            saveWindowGeometryToSettings();
            settings->save();
        }
        uiState_->kiosk = true;
        settings->setReadOnly(true);   // covers every UI save site
        pom2::log().info("Kiosk", "entered (full-screen, chrome-free, "
                                  "settings read-only)");
    } else {
        // Leaving kiosk. Release the host pointer first, before anything
        // touches the window. Kiosk is the mode where a captured pointer is
        // least of a problem (there is no UI to click), and the GUI is the
        // mode where it is most of one: the user comes back to menus, panels
        // and a docked layout, and every one of those needs a real cursor.
        // Doing it here rather than leaving it to the user also avoids a
        // GLFW_CURSOR_DISABLED pointer riding through the full-screen →
        // windowed monitor change, where the OS re-warps it. Entering kiosk
        // deliberately does NOT touch the grab — a captured mouse is what a
        // game in full screen wants.
        setMouseGrab(false);
        // Close the in-kiosk menu next so its captured
        // key handling doesn't leak into the GUI frame — and un-pause: the
        // menu pauses the machine while it is up, and leaving kiosk from an
        // open menu would otherwise strand the user in the GUI with a
        // silently stopped CPU.
        uiState_->kioskMenuOpen = false;
        // Undo only the pause the MENU imposed — kioskSetPaused keeps a
        // user-initiated pause intact (see uiState_->kioskPauseWasAlreadyStopped).
        kioskSetPaused(false);
        if (window) {
#ifdef __EMSCRIPTEN__
            // Nothing to restore: entering kiosk never moved the canvas (see
            // the matching guard above), and glfwSetWindowMonitor would
            // abort() the module here exactly as it does there. This is the
            // path the user actually hits — F10 to leave full-screen — so it
            // is the one that used to kill the emulator mid-session.
            pom2::log().info("Kiosk", "browser build — canvas size unchanged");
#else
            if (uiState_->savedWindowWidth > 0) {
                glfwSetWindowMonitor(window, nullptr, uiState_->savedWindowX, uiState_->savedWindowY,
                                     uiState_->savedWindowWidth, uiState_->savedWindowHeight, GLFW_DONT_CARE);
                // Many window managers IGNORE the position/size passed to
                // glfwSetWindowMonitor when leaving full-screen (they just
                // un-fullscreen and keep their own idea of the geometry) —
                // this is the standard GLFW workaround. Harmless when the
                // WM already honoured the call.
                glfwSetWindowSize(window, uiState_->savedWindowWidth, uiState_->savedWindowHeight);
                glfwSetWindowPos (window, uiState_->savedWindowX, uiState_->savedWindowY);
                if (uiState_->savedWindowMaximized) glfwMaximizeWindow(window);
                pom2::log().info("Kiosk",
                    "restored window " + std::to_string(uiState_->savedWindowWidth) + "x" +
                    std::to_string(uiState_->savedWindowHeight) + " at " +
                    std::to_string(uiState_->savedWindowX) + "," +
                    std::to_string(uiState_->savedWindowY) +
                    (uiState_->savedWindowMaximized ? " (maximized)" : ""));
            } else if (loadWindowGeometryFromSettings()) {
                // Launched with --kiosk: nothing was measured this session,
                // but a previous GUI session persisted its geometry.
                glfwSetWindowMonitor(window, nullptr, uiState_->savedWindowX, uiState_->savedWindowY,
                                     uiState_->savedWindowWidth, uiState_->savedWindowHeight, GLFW_DONT_CARE);
                glfwSetWindowSize(window, uiState_->savedWindowWidth, uiState_->savedWindowHeight);
                glfwSetWindowPos (window, uiState_->savedWindowX, uiState_->savedWindowY);
                if (uiState_->savedWindowMaximized) glfwMaximizeWindow(window);
                pom2::log().info("Kiosk",
                    "restored window from settings " +
                    std::to_string(uiState_->savedWindowWidth) + "x" +
                    std::to_string(uiState_->savedWindowHeight));
            } else {
                // Never ran windowed on this machine: centred default.
                GLFWmonitor* mon = glfwGetPrimaryMonitor();
                const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
                const int w = 1280, h = 850;
                const int x = vm ? (vm->width  - w) / 2 : 64;
                const int y = vm ? (vm->height - h) / 2 : 64;
                glfwSetWindowMonitor(window, nullptr, x, y, w, h, GLFW_DONT_CARE);
                glfwSetWindowSize(window, w, h);
                glfwSetWindowPos (window, x, y);
                uiState_->savedWindowX = x; uiState_->savedWindowY = y; uiState_->savedWindowWidth = w; uiState_->savedWindowHeight = h;
            }
#endif
        }
        uiState_->kiosk = false;
        // A session LAUNCHED with --kiosk stays read-only for life (the
        // documented "can't disturb your desktop setup" promise); a GUI
        // session that merely visited kiosk resumes writing.
        settings->setReadOnly(uiState_->launchedInKiosk);
        pom2::log().info("Kiosk", "left (windowed, full UI)");
    }
}

bool MainWindow::toggleKioskMode()
{
    setKioskModeRuntime(!uiState_->kiosk);
    return uiState_->kiosk;
}

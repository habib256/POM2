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

// Same heavy-includes-here pattern as MainWindow.cpp — MainWindow.h
// forward-declares the controller / cards / panels.
#include "AiControlServer.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CffaCard.h"
#include "CharRomCatalog.h"
#include "ClockCard.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "EmulationController.h"
#include "LeChatMauveCard.h"
#include "Logger.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "MouseCard.h"
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

// Persist a media bay's state with the right key scheme for its card type
// (SmartPort per-unit, CFFA per-slot, synthetic HDV a legacy global key).
// Called under stateMutex right after a mount/eject/type/write-back action.
// Promoted to a member so renderSlotConfigPanel's media column can reuse it
// (was a lambda in the now-removed Slot Manager).
void MainWindow::persistMediaBay(int slot, int bay, SlotPeripheral* p)
{
    if (auto* sp = dynamic_cast<pom2::SmartPortCard*>(p)) {
        const std::string base = "smartport_slot" + std::to_string(slot) +
                                 "_unit" + std::to_string(bay);
        const pom2::SmartPortUnit* u = sp->unit(static_cast<size_t>(bay));
        settings->setString(base + "_type",
                            u ? std::string(u->kindKey()) : std::string());
        settings->setString(base + "_path", u ? u->path() : std::string());
        settings->setBool  (base + "_writeback",
                            u ? u->isWriteBackEnabled() : false);
    } else if (auto* cffa = dynamic_cast<pom2::CffaCard*>(p)) {
        const std::string base = "cffa_slot" + std::to_string(slot);
        settings->setString(base + "_path", cffa->getImagePath());
        settings->setBool  (base + "_writeback", cffa->isWriteBackEnabled());
    } else if (auto* hdv = dynamic_cast<ProDOSHardDiskCard*>(p)) {
        settings->setString("hdv_path", hdv->getImagePath());
        settings->setBool  ("hdv_writeback", hdv->isWriteBackEnabled());
        hdvPath   = hdv->getImagePath();
        hdvStatus = hdv->isImageLoaded()
                      ? ("loaded: " + hdv->getImagePath())
                      : std::string("no image mounted");
    }
}

void MainWindow::renderSlotConfigPanel()
{
    if (!showSlotConfigPanel) return;

    // 880 px was sized for two columns; one column needs about half that.
    ImGui::SetNextWindowSize(ImVec2(520, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Slot Configuration", &showSlotConfigPanel)) {
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

        // Snapshot the canonical mapping into a working copy so the user's
        // edits are local until Apply.
        static std::array<std::string, 8> draft{};
        // Re-seed the working copy from the live slotCards[] whenever a
        // profile switch / settings restart rebuilt them. slotDraftInited_ is
        // a MainWindow member that applyProfile/restartEmulationFromSettings
        // reset for exactly this purpose — a plain `static bool` here would
        // never observe the rebuild and would keep stale assignments.
        if (!slotDraftInited_) {
            for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
            slotDraftInited_ = true;
        }

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
            const bool staged = (draft[s] != slotCards[s]);
            if (staged) {
                ImGui::PushStyleColor(ImGuiCol_Text,
                    ImGui::ColorConvertU32ToFloat4(pom2::palette().accent));
                ImGui::TextUnformatted(ICON_FA_CIRCLE_DOT);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) {
                    const char* wasLabel = "(empty)";
                    for (const auto& ct : kCardTypes)
                        if (ct.key == slotCards[s]) { wasLabel = ct.label; break; }
                    ImGui::SetTooltip("Staged. Currently plugged: %s", wasLabel);
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
            if (draft[s] != slotCards[s]) ++pending;
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
                      pending > 0 ? "Apply %d change%s (restarts emulator)"
                                  : "Apply (restarts emulator)",
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
                tapeStatusMessage = "Slot changes not applied — settings could not be saved.";
                tapeStatusUntil = lastFrameTime + 8.0;
                pom2::log().warn("Slots", tapeStatusMessage);
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
                    tapeStatusMessage =
                        "Slots applied, but updated settings could not be saved.";
                    tapeStatusUntil = lastFrameTime + 8.0;
                    pom2::log().warn("Slots", tapeStatusMessage);
                }
                for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
            }
        }
        ImGui::EndDisabled();
        if (pending > 0 && ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Restarts the emulated machine with the new cards.\n"
                "Mounted media is preserved where the card still exists.\n"
                "Only affects the slot list above — anything mounted from\n"
                "Internal Disks & Media has already taken effect.");
        ImGui::SameLine();
        ImGui::BeginDisabled(pending == 0);
        if (ImGui::Button("Revert"))
            for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
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
    if (!showMediaPanel) return;

    ImGui::SetNextWindowSize(ImVec2(520, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Internal Disks & Media", &showMediaPanel)) {
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
                            pom2::cardLabelForKey(slotCards[s]),
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
                                    {
                                        std::lock_guard<std::mutex> lk(controller->stateMutex());
                                        media->setBayType(b, o.first);
                                        persistMediaBay(s, b, p);
                                    }
                                    settings->save();
                                    mPrimed[s][b] = false;
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
                        std::string err;
                        bool ok = false;
                        {
                            std::lock_guard<std::mutex> lk(controller->stateMutex());
                            ok = media->mountBay(b, buf, err);
                            if (ok) persistMediaBay(s, b, p);
                        }
                        if (ok) settings->save();
                        tapeStatusMessage = ok
                            ? ("Slot " + std::to_string(s) + ": mounted " + buf)
                            : ("Slot " + std::to_string(s) + ": mount failed: " + err);
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!info.loaded);
                    if (ImGui::Button("Eject")) {
                        bool ok = false;
                        std::string err;
                        {
                            std::lock_guard<std::mutex> lk(controller->stateMutex());
                            ok = media->ejectBay(b);
                            if (ok) persistMediaBay(s, b, p);
                            else err = media->bayInfo(b).lastError;
                        }
                        if (ok) {
                            settings->save();
                            mPrimed[s][b] = false;
                        }
                        tapeStatusMessage = "Slot " + std::to_string(s) +
                            (ok ? ": ejected" : ": eject failed: " + err);
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();

                    if (info.supportsWriteBack) {
                        bool wb = info.writeBackEnabled;
                        ImGui::BeginDisabled(!typeAllows);
                        if (ImGui::Checkbox("Write-back (save on eject)", &wb)) {
                            {
                                std::lock_guard<std::mutex> lk(controller->stateMutex());
                                media->setBayWriteBack(b, wb);
                                persistMediaBay(s, b, p);
                            }
                            settings->save();
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
                    tapeStatusMessage = "Booting slot " + std::to_string(s);
                    tapeStatusUntil = lastFrameTime + 3.0;
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
                            pom2::cardLabelForKey(slotCards[s]),
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
                        bool ok = false;
                        {
                            std::lock_guard<std::mutex> lk(controller->stateMutex());
                            ok = d2->insertDisk(drv, buf);
                            if (ok) d2->seekTrack0();
                        }
                        // Only drive 1 has a persisted path key (disk_path_slotN);
                        // drive 2 mounts are session-only (matches legacy scheme).
                        if (ok && drv == 0) {
                            settings->setString(
                                "disk_path_slot" + std::to_string(s), std::string(buf));
                            settings->save();
                        }
                        tapeStatusMessage = ok
                            ? ("Slot " + std::to_string(s) + " drive " +
                               std::to_string(drv + 1) + ": inserted")
                            : ("Slot " + std::to_string(s) + ": insert failed: " +
                               d2->getLastError(drv));
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!loaded);
                    if (ImGui::Button("Eject")) {
                        bool ok = false;
                        {
                            std::lock_guard<std::mutex> lk(controller->stateMutex());
                            ok = d2->ejectDisk(drv);
                        }
                        if (ok && drv == 0) {
                            settings->setString(
                                "disk_path_slot" + std::to_string(s), std::string());
                            settings->save();
                        }
                        if (ok) dPrimed[s][drv] = false;
                        tapeStatusMessage = "Slot " + std::to_string(s) +
                            " drive " + std::to_string(drv + 1) +
                            (ok ? ": ejected" : ": eject failed: " +
                                  d2->getLastError(drv));
                        tapeStatusUntil = lastFrameTime + 4.0;
                    }
                    ImGui::EndDisabled();
                    ImGui::Unindent();
                    ImGui::PopID();
                }

                ImGui::BeginDisabled(!bootable);
                if (ImGui::SmallButton("Boot slot")) {
                    controller->bootFromSlot(s);
                    tapeStatusMessage = "Booting slot " + std::to_string(s);
                    tapeStatusUntil = lastFrameTime + 3.0;
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
        if (!kiosk_ && loadWindowGeometryFromSettings()) {
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
                if (savedWinX_ >= mx - 32 && savedWinX_ <= mx + mw - 64 &&
                    savedWinY_ >= my - 32 && savedWinY_ <= my + mh - 64)
                    onSomeMonitor = true;
            }
            GLFWmonitor* mon = glfwGetPrimaryMonitor();
            const GLFWvidmode* vm = mon ? glfwGetVideoMode(mon) : nullptr;
            if (vm) {
                if (savedWinW_ > vm->width)  savedWinW_ = vm->width;
                if (savedWinH_ > vm->height) savedWinH_ = vm->height;
            }
            if (!onSomeMonitor && vm) {
                // Saved on a since-disconnected screen — recentre on
                // primary rather than reopening off-screen.
                savedWinX_ = (vm->width  - savedWinW_) / 2;
                savedWinY_ = (vm->height - savedWinH_) / 2;
            }
            glfwSetWindowSize(window, savedWinW_, savedWinH_);
            glfwSetWindowPos (window, savedWinX_, savedWinY_);
            if (savedWinMaximized_) glfwMaximizeWindow(window);
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
        tapeStatusMessage = "Profile switch refused — save failed: " + flushErr;
        tapeStatusUntil = lastFrameTime + 8.0;
        pom2::log().warn("Profile", tapeStatusMessage);
        if (wasRunning) controller->start();
        return;
    }

    // The session-local auto-plugged HDV (POM2 <image.hdv> one-shot boot) is
    // destroyed by the slot rebuild below; clear its marker so a later real
    // HDV in the same slot number isn't wrongly skipped at shutdown.
    autoProvisionedHdvSlot_ = -1;
    autoProvisionedSmartPortSlot_ = -1;

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
    // The rewind ring recorded the PREVIOUS machine: steps below wipe
    // RAM/aux/ROM and rebuild the card set, so an F6 restore after the
    // switch would push the old machine's RAM/CPU/slot state onto the new
    // hardware (II+ Applesoft PC on a //e ROM → crash). Only coldBoot
    // cleared it before.
    controller->rewind().clear();

    // 2. Snapshot the currently-mounted media so we can re-mount after
    //    the cold reset. The user wants to test the same disk under
    //    different machine models; everything else (CPU state, RAM,
    //    soft switches) is wiped intentionally.
    //
    //    Read the LIVE card state (not `settings->getString("disk_path")`
    //    which is only written to disk in the MainWindow dtor) — so a
    //    disk inserted mid-session via the Disk II / HDV panel survives
    //    a profile switch. Skip the synthesised host-folder HDV volume
    //    (its "path" is a `[host folder] <dir>` sentinel, not a real
    //    file) since `loadImage` would fail on the sentinel; the user
    //    can re-synthesise from the Library after the switch.
    //
    //    Built under stateMutex and copied BY VALUE. `controller->stop()`
    //    above parks the CPU worker but nothing quiesces the AI control
    //    server's HTTP thread, whose /disk insert + eject handlers reassign
    //    the very std::string that getDiskPath()/getImagePath() return a
    //    reference into. aiServer->detach() only happens in step 3, below.
    std::string savedHdvPath;
    // Capture every plugged Disk II's path so multi-instance setups
    // (DiskII slot 6 + DiskII slot 4) survive the profile switch. Indexed
    // by slot number, not by diskCards[] order, so re-plugging in the
    // same slot picks the right path even if SettingsBackedSlots returns
    // them in a different order.
    // Pair the path with the live write-back toggle, same as savedCffaPaths
    // below: plugSlotsFromSettings restores the *persisted* opt-in, but a
    // toggle made from the Disk II panel this session isn't in settings yet
    // and would be silently reverted to read-only by the re-plug.
    // `nullopt` = no Disk II in that slot before the switch, so the card the
    // new profile plugs there keeps whatever plugSlotsFromSettings restored
    // from the persisted keys instead of inheriting a fabricated read-only.
    std::array<std::optional<std::pair<std::string, bool>>, 8> savedDiskPaths{};
    // Same idea for CFFA: a Disk-Library mid-session mount only updates the
    // live card, not settings, so plugSlotsFromSettings's cffa_slotN_path
    // restore would otherwise silently revert to the pre-session path (or
    // drop the mount entirely if there wasn't one). Pair the path with the
    // user's write-back toggle — re-plug defaults to read-only.
    std::array<std::pair<std::string, bool>, 8> savedCffaPaths{};
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (auto* c : diskCards) {
            if (!c) continue;
            savedDiskPaths[static_cast<size_t>(c->getSlot())] =
                std::make_pair(c->isDiskLoaded() ? c->getDiskPath() : std::string(),
                               c->isWriteBackEnabled());
        }
        if (hdvCard && hdvCard->isImageLoaded()) {
            const std::string path = hdvCard->getImagePath();
            if (path.rfind("[host folder] ", 0) == std::string::npos) {
                savedHdvPath = path;
            }
        }
        for (auto* blk : blockCards()) {
            auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
            if (!cffa || !cffa->isImageLoaded()) continue;
            savedCffaPaths[static_cast<size_t>(cffa->getSlot())] =
                { cffa->getImagePath(), cffa->isWriteBackEnabled() };
        }
    }

    // 3. Tear down all slot cards under the state mutex. Mockingboard's
    //    AudioSource must be detached BEFORE the slot bus destroys the
    //    card (the audio thread's next callback would dereference a
    //    freed source otherwise — same gotcha as restartEmulationFromSettings).
    {
        auto st = controller->lockState();
        // First: null the AI control server's card pointers under the
        // same lock that handlers grab. A request that already passed
        // its lock acquisition is using the still-alive card; later
        // requests will see null and return 503. We re-attach at the
        // end after the new cards are in place.
        aiServer->detach();
        // Any card that registered an AudioSource with the audio device
        // must be unregistered BEFORE slotBus().clear() destroys it —
        // otherwise the audio thread's next callback dereferences a
        // freed source. Drive this off the registration inventory, not the
        // `*Card` aliases: those are last-one-wins and a config with two
        // Mockingboard variants (A/C + Sound II) left the first card's
        // source registered against freed memory. Mirrored in
        // restartEmulationFromSettings.
        unregisterAllAudioSources();
        diskCard         = nullptr;
        diskCards.clear();
        diskPanels.clear();
        diskPanel        = nullptr;
        hdvCard          = nullptr;
        cffaCard         = nullptr;
        chatMauveCard    = nullptr;
        sscCard          = nullptr;
        sscCards.clear();
        clockCard        = nullptr;
        mouseCard        = nullptr;
        mouseAwCard      = nullptr;
        mockingboardCard = nullptr;
        phasorCard       = nullptr;
        echoPlusCard     = nullptr;
        echoPlusTmsCard  = nullptr;
        printerCard      = nullptr;
        // grapplerCard feeds pumpImageWriter() every frame — leaving it
        // dangling here is a use-after-free the moment the card is gone.
        grapplerCard     = nullptr;
        // Same hazard: the Ethernet panel dereferences these every frame.
        uthernetCard     = nullptr;
        uthernetIICard   = nullptr;
        // The FujiNet card owns a listening socket / open serial device and
        // a worker thread; slotBus().clear() destroys it, which joins the
        // thread. Dropping our alias first keeps the panel from touching a
        // card that is mid-teardown.
        fujiNetCard      = nullptr;
        smartPortCard    = nullptr;
        st.memory().slotBus().clear();
        display->setChatMauveCard(nullptr);

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
    // Force the Slot Config panel to re-seed its draft from the rebuilt
    // slotCards[] on its next render (stale-draft-after-profile-switch fix).
    slotDraftInited_ = false;

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
        if (builtinRgb && chatMauveCard)
            display->setHiResMode(Apple2Display::HiResMode::ChatMauveRGB);
    }

    // 8. Re-mount preserved media. Iterate every newly-plugged DiskII
    //    and look up its slot in the snapshot — empty entries mean no
    //    disk was mounted there at the profile-switch time.
    for (auto* c : diskCards) {
        if (!c) continue;
        const auto& saved = savedDiskPaths[static_cast<size_t>(c->getSlot())];
        if (!saved) continue;               // no DiskII here before the switch
        const auto& [path, writeBack] = *saved;
        // Write-back BEFORE the mount: insertDisk copies the card flag into
        // the freshly loaded image, and a card that comes up read-only makes
        // the guest see a write-protected disk (DOS 3.3 "WRITE PROTECTED",
        // Print Shop hanging on its setup save).
        c->setWriteBackEnabled(writeBack);
        if (path.empty()) continue;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            (void)c->insertDisk(path);
        }
    }
    if (hdvCard && !savedHdvPath.empty()) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(savedHdvPath, ec)) {
            (void)hdvCard->loadImage(savedHdvPath);
        }
    }
    // CFFA: plugSlotsFromSettings already mounted whatever `cffa_slotN_path`
    // settings held; if the user mounted a different image mid-session, the
    // live snapshot wins (matches Disk II / HDV above). Empty snapshot ⇒
    // leave plugSlots' settings-driven mount alone.
    for (auto* blk : blockCards()) {
        auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
        if (!cffa) continue;
        const auto& [path, wb] = savedCffaPaths[static_cast<size_t>(cffa->getSlot())];
        if (path.empty()) continue;
        std::error_code ec;
        if (std::filesystem::is_regular_file(path, ec)) {
            (void)cffa->loadImage(path);
            cffa->setWriteBackEnabled(wb);
        }
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
    if (!kiosk_) {
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

    // Re-bind the AI control server to the freshly rebuilt slot pointers.
    // (Profile switch rebuilds the SlotBus; diskCard/hdvCard pointers from
    // the previous profile are stale.) Held under stateMutex so a
    // handler observing the pointers between detach() and now sees the
    // null (→ 503) rather than a torn intermediate state.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
    }
    aiServer->setProfileLabel(std::string(cfg.displayName));
}

bool MainWindow::restartEmulationFromSettings()
{
    // 0. Snapshot LIVE media into settings BEFORE teardown. Menu Insert/Eject
    //    and the HDV/CFFA library mounts update the live cards but NOT the
    //    settings keys (those are written only at shutdown), so without this a
    //    Slot-Config "Apply" rebuilds from stale keys and silently drops the
    //    mounted disk/HDV/CFFA. plugSlotsFromSettings + step 4 restore FROM
    //    settings, so persisting the live state here preserves it.
    //
    //    Read under stateMutex and copy BY VALUE: getDiskPath() /
    //    getImagePath() hand back a reference into the card's live
    //    DiskImage, and the AI control server's HTTP thread (which nothing
    //    has parked yet — aiServer->detach() is step 2) reassigns exactly
    //    that string from its /disk insert + eject handlers.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        for (auto* c : diskCards) {
            if (!c) continue;
            const std::string slotKey = "_slot" + std::to_string(c->getSlot());
            settings->setString("disk_path" + slotKey,
                c->isDiskLoaded() ? std::string(c->getDiskPath()) : std::string());
            settings->setBool("disk_writeback" + slotKey, c->isWriteBackEnabled());
        }
        if (hdvCard && hdvCard->getSlot() != autoProvisionedHdvSlot_ &&
            hdvCard->isImageLoaded()) {
            const std::string p = hdvCard->getImagePath();
            if (p.rfind("[host folder] ", 0) == std::string::npos)
                settings->setString("hdv_path", p);
        }
        for (auto* blk : blockCards()) {
            auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
            if (!cffa) continue;
            const std::string key = "cffa_slot" + std::to_string(cffa->getSlot());
            settings->setString(key + "_path",
                cffa->isImageLoaded() ? std::string(cffa->getImagePath())
                                      : std::string());
            settings->setBool(key + "_writeback", cffa->isWriteBackEnabled());
        }
    }
    // (3.5"/SmartPort media is already saved eagerly on mount.)

    // 1. Stop the worker thread so card destructors don't race against a
    //    running CPU step, then prove every opted-in dirty medium is durable
    //    before destroying any card.
    const auto previousMode = controller->getMode();
    controller->stop();
    std::string flushErr;
    if (!flushSlotMedia(flushErr)) {
        tapeStatusMessage = "Slot rebuild refused — save failed: " + flushErr;
        tapeStatusUntil = lastFrameTime + 8.0;
        pom2::log().warn("Slots", tapeStatusMessage);
        controller->setMode(previousMode);
        return false;
    }
    // The rebuild is now committed. Its session-local auto-plugged media will
    // be destroyed below, so their shutdown-persistence markers no longer
    // describe a live card.
    autoProvisionedHdvSlot_ = -1;
    autoProvisionedSmartPortSlot_ = -1;
    // Drop the rewind ring: its SLOTn sections describe the card set being
    // torn down; restoring one onto the rebuilt (possibly different) cards
    // would be incoherent. Same rationale as applyProfile.
    controller->rewind().clear();

    // 2. Tear down all cards and clear our raw pointers. Holding the
    //    state mutex isn't strictly necessary now that the worker is
    //    stopped, but it's cheap insurance against any UI thread that
    //    might be peeking — AND it serialises with the AI control
    //    server's handlers (which take the same mutex around card
    //    pointer reads). aiServer->detach() must happen under this
    //    lock to safely null disk6_/hdv5_ before slotBus.clear()
    //    destroys their pointees.
    {
        auto st = controller->lockState();
        aiServer->detach();
        // Every card that owns an AudioSource (Mockingboard / Phasor /
        // Echo+) must be unregistered from the audio device BEFORE the
        // slot bus destroys it — otherwise the audio thread's next
        // callback dereferences a freed source. The inventory covers every
        // registered source, including the second of two coexisting
        // Mockingboard variants that the single `mockingboardCard` alias
        // cannot represent. Same gotcha mirrored in applyProfile's teardown.
        unregisterAllAudioSources();
        diskCard         = nullptr;
        diskCards.clear();
        diskPanels.clear();
        diskPanel        = nullptr;
        hdvCard          = nullptr;
        cffaCard         = nullptr;
        chatMauveCard    = nullptr;
        sscCard          = nullptr;
        sscCards.clear();
        clockCard        = nullptr;
        mouseCard        = nullptr;
        mouseAwCard      = nullptr;
        mockingboardCard = nullptr;
        phasorCard       = nullptr;
        echoPlusCard     = nullptr;
        echoPlusTmsCard  = nullptr;
        printerCard      = nullptr;
        grapplerCard     = nullptr;   // see pumpImageWriter() — non-owning
        uthernetCard     = nullptr;   // see the Ethernet panel — non-owning
        uthernetIICard   = nullptr;
        fujiNetCard      = nullptr;   // owns a socket + worker thread
        smartPortCard    = nullptr;
        st.memory().slotBus().clear();
        // Also drop any cached display->setChatMauveCard pointer — the
        // next plug call will set it again.
        display->setChatMauveCard(nullptr);
    }

    // 3-4 run under stateMutex — same rationale as applyProfile steps
    // 5-7: the AI control server's handlers still run against
    // controller/memory (detach() nulled only its card pointers), so the
    // SlotBus rebuild + remounts must be atomic w.r.t. their lock.
    {
    auto st = controller->lockState();

    // 3. Re-run plugSlotsFromSettings() with the freshly-saved keys.
    plugSlotsFromSettings(st);
    // Re-seed the Slot Config draft from the rebuilt slotCards[] next render.
    slotDraftInited_ = false;

    // 4. Restore each DiskII's persisted state (matches MainWindow ctor).
    //    Per-slot keys for multi-instance configs. Legacy `disk_path` /
    //    `disk_writeback` (no slot suffix) is still read as the fallback
    //    for the primary (lowest-slot) card so settings.ini files written
    //    before option C 2026-05-15 keep working.
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string slotKey = "_slot" + std::to_string(c->getSlot());
        const bool isPrimary = (c == diskCard);
        const bool wb = settings->getBool(
            "disk_writeback" + slotKey,
            isPrimary ? settings->getBool("disk_writeback", false) : false);
        c->setWriteBackEnabled(wb);
        const std::string diskPath = settings->getString(
            "disk_path" + slotKey,
            isPrimary ? settings->getString("disk_path", "") : std::string());
        std::error_code ec;
        if (!diskPath.empty() &&
            std::filesystem::is_regular_file(diskPath, ec)) {
            (void)c->insertDisk(diskPath);
        }
    }

    }   // end stateMutex scope over steps 3-4

    // 5. Hard reset + restart worker. Route through `controller->hardReset()`
    //    rather than `cpu().hardReset()` + `slotBus().reset()` — the
    //    controller path additionally disarms `iicSmartPortArmed_` (via
    //    `Memory::setIicSmartPortArmed(false)`) and resets the speaker /
    //    IWM / SmartPort hub. Pre-fix: on //c-class, the $C500 firmware
    //    punch stayed armed after `bootFromSlot(5)`, so the post-Apply
    //    reset vector fetched while the punch was live → //c F8 autostart
    //    re-booted SmartPort instead of leaving the user at the BASIC
    //    prompt the Apply was meant to give them.
    controller->hardReset();
    controller->start();

    // 6. Re-attach the AI control server with the freshly rebuilt card
    //    pointers — the slot-bus tear-down above invalidated whatever
    //    diskCard/hdvCard the server was holding. Held under stateMutex
    //    so any handler that observed the detached null sees the new
    //    pointers atomically with respect to its own lock acquisition.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        aiServer->attach(controller.get(), display.get(), diskCard, hdvCard);
    }

    pom2::log().info("Slots", "Emulator restarted with new slot mapping.");
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
    if (savedWinW_ <= 0 || !settings) return;
    settings->setInt ("window_x", savedWinX_);
    settings->setInt ("window_y", savedWinY_);
    settings->setInt ("window_w", savedWinW_);
    settings->setInt ("window_h", savedWinH_);
    settings->setBool("window_maximized", savedWinMaximized_);
}

bool MainWindow::loadWindowGeometryFromSettings()
{
    if (!settings) return false;
    const int w = settings->getInt("window_w", 0);
    const int h = settings->getInt("window_h", 0);
    if (w <= 0 || h <= 0) return false;
    savedWinX_ = settings->getInt("window_x", 0);
    savedWinY_ = settings->getInt("window_y", 0);
    savedWinW_ = w;
    savedWinH_ = h;
    savedWinMaximized_ = settings->getBool("window_maximized", false);
    return true;
}

void MainWindow::setKioskMode(bool k)
{
    kiosk_           = k;
    launchedInKiosk_ = k;
    if (k && settings) settings->setReadOnly(true);
}

void MainWindow::captureWindowGeometryNow()
{
    if (!window || kiosk_ || settingsReadOnly()) return;
    // A MAXIMIZED window reports the maximized rect. Do NOT un-maximize to
    // measure: on X11 glfwRestoreWindow only posts a _NET_WM_STATE message
    // and returns, so the very next query still reads the maximized rect —
    // and we would have un-maximized the user's window for nothing. Record
    // the flag and KEEP whatever non-maximized geometry we already had
    // (from an earlier capture or from settings), so re-maximizing on
    // restore lands correctly and un-maximizing afterwards gives a sane
    // floating size instead of a screen-sized rectangle.
    const bool maximized = glfwGetWindowAttrib(window, GLFW_MAXIMIZED) != 0;
    savedWinMaximized_ = maximized;
    if (!maximized) {
        glfwGetWindowPos(window, &savedWinX_, &savedWinY_);
        glfwGetWindowSize(window, &savedWinW_, &savedWinH_);
    }
    saveWindowGeometryToSettings();
}

void MainWindow::setKioskModeRuntime(bool k)
{
    if (k == kiosk_) return;

    if (k) {
        // Entering kiosk. Persist first: kiosk deliberately never writes
        // state.cfg, so anything the user changed in the GUI session would
        // otherwise be lost if they quit from kiosk. (A session LAUNCHED
        // with --kiosk was read-only from the start and stays that way —
        // see settingsReadOnly().)
        if (window) {
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
        kiosk_ = true;
        settings->setReadOnly(true);   // covers every UI save site
        pom2::log().info("Kiosk", "entered (full-screen, chrome-free, "
                                  "settings read-only)");
    } else {
        // Leaving kiosk. Close the in-kiosk menu first so its captured
        // key handling doesn't leak into the GUI frame — and un-pause: the
        // menu pauses the machine while it is up, and leaving kiosk from an
        // open menu would otherwise strand the user in the GUI with a
        // silently stopped CPU.
        kioskMenuOpen_ = false;
        // Undo only the pause the MENU imposed — kioskSetPaused keeps a
        // user-initiated pause intact (see kioskPauseWasAlreadyStopped_).
        kioskSetPaused(false);
        if (window) {
            if (savedWinW_ > 0) {
                glfwSetWindowMonitor(window, nullptr, savedWinX_, savedWinY_,
                                     savedWinW_, savedWinH_, GLFW_DONT_CARE);
                // Many window managers IGNORE the position/size passed to
                // glfwSetWindowMonitor when leaving full-screen (they just
                // un-fullscreen and keep their own idea of the geometry) —
                // this is the standard GLFW workaround. Harmless when the
                // WM already honoured the call.
                glfwSetWindowSize(window, savedWinW_, savedWinH_);
                glfwSetWindowPos (window, savedWinX_, savedWinY_);
                if (savedWinMaximized_) glfwMaximizeWindow(window);
                pom2::log().info("Kiosk",
                    "restored window " + std::to_string(savedWinW_) + "x" +
                    std::to_string(savedWinH_) + " at " +
                    std::to_string(savedWinX_) + "," +
                    std::to_string(savedWinY_) +
                    (savedWinMaximized_ ? " (maximized)" : ""));
            } else if (loadWindowGeometryFromSettings()) {
                // Launched with --kiosk: nothing was measured this session,
                // but a previous GUI session persisted its geometry.
                glfwSetWindowMonitor(window, nullptr, savedWinX_, savedWinY_,
                                     savedWinW_, savedWinH_, GLFW_DONT_CARE);
                glfwSetWindowSize(window, savedWinW_, savedWinH_);
                glfwSetWindowPos (window, savedWinX_, savedWinY_);
                if (savedWinMaximized_) glfwMaximizeWindow(window);
                pom2::log().info("Kiosk",
                    "restored window from settings " +
                    std::to_string(savedWinW_) + "x" +
                    std::to_string(savedWinH_));
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
                savedWinX_ = x; savedWinY_ = y; savedWinW_ = w; savedWinH_ = h;
            }
        }
        kiosk_ = false;
        // A session LAUNCHED with --kiosk stays read-only for life (the
        // documented "can't disturb your desktop setup" promise); a GUI
        // session that merely visited kiosk resumes writing.
        settings->setReadOnly(launchedInKiosk_);
        pom2::log().info("Kiosk", "left (windowed, full UI)");
    }
}

bool MainWindow::toggleKioskMode()
{
    setKioskModeRuntime(!kiosk_);
    return kiosk_;
}

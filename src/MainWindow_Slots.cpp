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

    ImGui::SetNextWindowSize(ImVec2(880, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Slot Configuration", &showSlotConfigPanel)) {
        ImGui::End();
        return;
    }

    ImGui::TextWrapped(
        "Left: assign a card to each expansion slot — cards built into the "
        "active profile are locked and shown greyed. Right: mount media into "
        "the internal disks and the mountable ports of the storage cards that "
        "are plugged.");
    ImGui::Spacing();

    const auto& profileCfg = pom2::profileConfig(activeProfile);

    // ══ LEFT COLUMN: per-slot card assignment ════════════════════════════
    ImGui::BeginChild("##slotassign", ImVec2(400, 0), true);
    {
        ImGui::SeparatorText("Expansion slots");

        // Snapshot the canonical mapping into a working copy so the user's
        // edits are local until Apply.
        static std::array<std::string, 8> draft{};
        static bool draftInited = false;
        if (!draftInited) {
            for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
            draftInited = true;
        }

        const bool mouseAvailable    = mouseRomsPresent();
        const bool mouseAwAvailable  = mouseAwRomPresent();
        const bool cffaAvailable     = cffaRomPresent();

        // AUX slot (IIe-class only): built-in 80-column card at $C300 — shown
        // greyed as a non-editable row.
        if (profileCfg.iieMode) {
            ImGui::BeginDisabled(true);
            ImGui::LabelText("AUX slot", "%s",
                "Extended 80-Column Card (built-in, $C300 firmware)");
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
                ImGui::LabelText(label, "%s", preview);
                ImGui::EndDisabled();
                continue;
            }

            // Profile has no physical expansion bus (//c / //c+): even the
            // "empty" virtual slots aren't pluggable on real hardware. Show
            // the row greyed-out so the user understands why.
            if (profileCfg.noPhysicalSlots) {
                draft[s] = "";
                ImGui::BeginDisabled(true);
                ImGui::LabelText(label, "(no physical slot on %s)",
                                 std::string(profileCfg.displayName).c_str());
                ImGui::EndDisabled();
                continue;
            }

            const bool dup = isDuplicate(s);
            if (dup) anyDuplicate = true;

            const char* preview = "(empty)";
            for (const auto& ct : kCardTypes) {
                if (ct.key == draft[s]) { preview = ct.label; break; }
            }

            if (dup) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 96, 96, 255));
            if (ImGui::BeginCombo(label, preview)) {
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
        ImGui::BeginDisabled(anyDuplicate);
        if (ImGui::Button("Apply (restarts emulator)")) {
            for (int s = 1; s <= 7; ++s)
                settings->setString("slot_" + std::to_string(s) + "_card", draft[s]);
            settings->save();
            restartEmulationFromSettings();
            for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Revert"))
            for (int s = 1; s <= 7; ++s) draft[s] = slotCards[s];
    }
    ImGui::EndChild();

    ImGui::SameLine();

    // ══ RIGHT COLUMN: internal disks + mountable ports ═══════════════════
    ImGui::BeginChild("##slotmedia", ImVec2(0, 0), true);
    {
        ImGui::SeparatorText("Internal disks & mountable ports");

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
                    const pom2::MediaBayInfo info = media->bayInfo(b);
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
                        {
                            std::lock_guard<std::mutex> lk(controller->stateMutex());
                            media->ejectBay(b);
                            persistMediaBay(s, b, p);
                        }
                        settings->save();
                        mPrimed[s][b] = false;
                        tapeStatusMessage = "Slot " + std::to_string(s) + ": ejected";
                        tapeStatusUntil = lastFrameTime + 3.0;
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
                    const bool loaded = d2->isDiskLoaded(drv);
                    if (drv == 0 && loaded) bootable = true;
                    ImGui::PushID(drv);
                    ImGui::Indent();
                    dot(loaded, false);
                    ImGui::Text("Drive %d", drv + 1);

                    char* buf = dBuf[s][drv].data();
                    if (!dPrimed[s][drv]) {
                        std::snprintf(buf, dBuf[s][drv].size(), "%s",
                                      d2->getDiskPath(drv).c_str());
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
                        {
                            std::lock_guard<std::mutex> lk(controller->stateMutex());
                            d2->ejectDisk(drv);
                        }
                        if (drv == 0) {
                            settings->setString(
                                "disk_path_slot" + std::to_string(s), std::string());
                            settings->save();
                        }
                        dPrimed[s][drv] = false;
                        tapeStatusMessage = "Slot " + std::to_string(s) +
                            " drive " + std::to_string(drv + 1) + ": ejected";
                        tapeStatusUntil = lastFrameTime + 3.0;
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
    if (override == "nmos")  return M6502::CpuMode::NMOS;
    if (override == "65c02") return M6502::CpuMode::CMOS;
    return profileDefault;     // "auto" (default) — follow the profile
}

float MainWindow::floppyMotorPitchForProfile(pom2::SystemProfile p)
{
    switch (p) {
        case pom2::SystemProfile::AppleIIc:
        case pom2::SystemProfile::AppleIIcPlus:
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
        std::string title = "POM2 v0.6 — ";
        title.append(cfg.displayName);
        glfwSetWindowTitle(window, title.c_str());
    }
}

void MainWindow::applyProfile(pom2::SystemProfile p)
{
    const auto& cfg = pom2::profileConfig(p);
    pom2::log().info("Profile",
        std::string("Switching to ") + std::string(cfg.displayName));

    // The session-local auto-plugged HDV (POM2 <image.hdv> one-shot boot) is
    // destroyed by the slot rebuild below; clear its marker so a later real
    // HDV in the same slot number isn't wrongly skipped at shutdown.
    autoProvisionedHdvSlot_ = -1;

    // 0. Commit the active profile NOW — BEFORE step 7's plugSlotsFromSettings(),
    //    which reads `activeProfile` to apply the profile's built-in locked slots
    //    (//c / //c+ on-board SSC / Mouse / SmartPort / Disk II). Setting it only
    //    at step 12 meant the re-plug used the PREVIOUS profile's built-ins:
    //    switching INTO //c/c+ never forced its on-board cards (no boot disk
    //    controller — also at startup, where the ctor calls applyProfile(saved)),
    //    and switching AWAY leaked //c built-ins into a clean II+/IIe. Everything
    //    between here and step 7 keys off the local `cfg`/`p`, not the member.
    activeProfile = p;

    // 1. Stop the worker thread (cards' destructors must not race a
    //    running CPU step or worker idle-loop probe).
    controller->stop();

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
    std::string savedHdvPath;
    // Capture every plugged Disk II's path so multi-instance setups
    // (DiskII slot 6 + DiskII slot 4) survive the profile switch. Indexed
    // by slot number, not by diskCards[] order, so re-plugging in the
    // same slot picks the right path even if SettingsBackedSlots returns
    // them in a different order.
    std::array<std::string, 8> savedDiskPaths{};
    for (auto* c : diskCards) {
        if (c && c->isDiskLoaded()) {
            savedDiskPaths[static_cast<size_t>(c->getSlot())] = c->getDiskPath();
        }
    }
    if (hdvCard && hdvCard->isImageLoaded()) {
        const std::string& path = hdvCard->getImagePath();
        if (path.rfind("[host folder] ", 0) == std::string::npos) {
            savedHdvPath = path;
        }
    }
    // Same idea for CFFA: a Disk-Library mid-session mount only updates the
    // live card, not settings, so plugSlotsFromSettings's cffa_slotN_path
    // restore would otherwise silently revert to the pre-session path (or
    // drop the mount entirely if there wasn't one). Pair the path with the
    // user's write-back toggle — re-plug defaults to read-only.
    std::array<std::pair<std::string, bool>, 8> savedCffaPaths{};
    for (auto* blk : blockCards()) {
        auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
        if (!cffa || !cffa->isImageLoaded()) continue;
        savedCffaPaths[static_cast<size_t>(cffa->getSlot())] =
            { cffa->getImagePath(), cffa->isWriteBackEnabled() };
    }

    // 3. Tear down all slot cards under the state mutex. Mockingboard's
    //    AudioSource must be detached BEFORE the slot bus destroys the
    //    card (the audio thread's next callback would dereference a
    //    freed source otherwise — same gotcha as restartEmulationFromSettings).
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        // First: null the AI control server's card pointers under the
        // same lock that handlers grab. A request that already passed
        // its lock acquisition is using the still-alive card; later
        // requests will see null and return 503. We re-attach at the
        // end after the new cards are in place.
        aiServer->detach();
        // Any card that registered an AudioSource with the audio device
        // must be unregistered BEFORE slotBus().clear() destroys it —
        // otherwise the audio thread's next callback dereferences a
        // freed source. Mirror this in restartEmulationFromSettings.
        if (controller->audio().isAvailable()) {
            if (mockingboardCard)
                controller->audio().removeSource(mockingboardCard->audioSource());
            if (phasorCard)
                controller->audio().removeSource(phasorCard->audioSource());
            if (echoPlusCard)
                controller->audio().removeSource(echoPlusCard->audioSource());
        }
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
        printerCard      = nullptr;
        smartPortCard    = nullptr;
        controller->memory().slotBus().clear();
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
        controller->memory().setIIEMode(cfg.iieMode);
        controller->memory().clearRam();
        controller->memory().resetSoftSwitches();

        // RamWorks III — Applied Engineering aux-slot RAM expansion.
        // Plugs into the IIe aux slot, present on BOTH the 1983 Unenhanced
        // and 1985 Enhanced //e; only //c and //c+ lack it (their aux RAM is
        // on the motherboard, no expansion bus). Gate on either //e variant
        // so $C073 writes on //c stay in the paddle-reset-only path. Tiers:
        // 1 (stock 64K), 4 (256K), 8 (512K), 16 (1M), 48 (3M), 128 (8M).
        // Default 1 = no RamWorks. The setIIEMode(false) branch already
        // cleared backing storage.
        if (p == pom2::SystemProfile::AppleIIe ||
            p == pom2::SystemProfile::AppleIIeUnenhanced) {
            const int banks = settings->getInt("ramworks_banks", 1);
            controller->memory().setRamWorksBanks(
                static_cast<uint32_t>(banks > 0 ? banks : 1));
        } else if (cfg.iieMode) {
            // //c / //c+ — force RamWorks off (might be left over from a
            // prior IIe-profile session). setRamWorksBanks(1) releases
            // the backing.
            controller->memory().setRamWorksBanks(1);
        }
    }

    // 5. Resolve and load the new main ROM.
    //    //c / //c+ 32 KB dumps are two firmware banks (bank 0 lower,
    //    bank 1 upper) where the //e 32 KB layout uses "char ROM lower,
    //    firmware upper" — same file size, opposite slicing. Tell the
    //    loader which way to slice based on the active profile.
    const bool pickLowerHalf =
        (p == pom2::SystemProfile::AppleIIc ||
         p == pom2::SystemProfile::AppleIIcPlus);
    const std::string newRomPath = firstExistingPath(cfg.romProbeOrder);
    if (!newRomPath.empty()
        && controller->memory().loadAppleIIRom(newRomPath.c_str(), pickLowerHalf)) {
        romPath  = newRomPath;
        romStatus = std::string(cfg.iieMode ? "IIe/IIc: " : "loaded: ") + newRomPath;
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
        controller->memory().loadCharRom(newCharPath.c_str());
    }
    if (cfg.iieMode) display->setAuxMemory(controller->memory().auxData());
    else             display->setAuxMemory(nullptr);

    // 7. Re-plug slot cards. plugSlotsFromSettings honours user's
    //    persisted slot config; the profile choice doesn't override that
    //    (e.g. a user who put SSC in slot 4 keeps it across profile
    //    switches).
    plugSlotsFromSettings();

    // 8. Re-mount preserved media. Iterate every newly-plugged DiskII
    //    and look up its slot in the snapshot — empty entries mean no
    //    disk was mounted there at the profile-switch time.
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string& path = savedDiskPaths[static_cast<size_t>(c->getSlot())];
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
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        controller->cpu().setCpuMode(resolveCpuMode(cfg.defaultCpu));
    }

    // 10. Default CPU pacing.
    controller->setCyclesPerFrame(cfg.defaultCyclesPerFrame);

    // 11. Final hard reset — CPU re-fetches PC from the new ROM's reset
    //     vector at $FFFC/$FFFD.
    controller->hardReset();
    controller->start();

    // 12. Persist the profile choice for the next launch. (activeProfile was
    //     already committed in step 0 so plugSlotsFromSettings saw the new one.)
    controller->floppySound525().setMotorPitch(floppyMotorPitchForProfile(p));
    settings->setString("system_profile", std::string(cfg.key));
    settings->save();

    // 13. Reflect the profile in the window title so the user sees which
    //     machine is active without opening the Machine → Profile menu.
    //     Skipped when called from the constructor (window not yet set
    //     by main.cpp's setGlfwWindow).
    if (window) {
        std::string title = "POM2 v0.6 — ";
        title.append(cfg.displayName);
        glfwSetWindowTitle(window, title.c_str());
    }

    pom2::log().info("Profile",
        std::string("Active = ") + std::string(cfg.displayName) +
        ", ROM = " + (newRomPath.empty() ? "<missing>" : newRomPath) +
        ", CPU = " +
        (controller->cpu().getCpuMode() == M6502::CpuMode::CMOS ? "65C02" : "NMOS"));

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

void MainWindow::restartEmulationFromSettings()
{
    // The session-local auto-plugged HDV is destroyed by the rebuild below;
    // clear its marker so a later real HDV in the same slot isn't wrongly
    // skipped at shutdown (the marker is read only in ~MainWindow).
    autoProvisionedHdvSlot_ = -1;

    // 0. Snapshot LIVE media into settings BEFORE teardown. Menu Insert/Eject
    //    and the HDV/CFFA library mounts update the live cards but NOT the
    //    settings keys (those are written only at shutdown), so without this a
    //    Slot-Config "Apply" rebuilds from stale keys and silently drops the
    //    mounted disk/HDV/CFFA. plugSlotsFromSettings + step 4 restore FROM
    //    settings, so persisting the live state here preserves it.
    for (auto* c : diskCards) {
        if (!c) continue;
        const std::string slotKey = "_slot" + std::to_string(c->getSlot());
        settings->setString("disk_path" + slotKey,
            c->isDiskLoaded() ? c->getDiskPath() : std::string());
        settings->setBool("disk_writeback" + slotKey, c->isWriteBackEnabled());
    }
    if (hdvCard && hdvCard->getSlot() != autoProvisionedHdvSlot_ &&
        hdvCard->isImageLoaded()) {
        const std::string& p = hdvCard->getImagePath();
        if (p.rfind("[host folder] ", 0) == std::string::npos)
            settings->setString("hdv_path", p);
    }
    for (auto* blk : blockCards()) {
        auto* cffa = dynamic_cast<pom2::CffaCard*>(blk);
        if (!cffa) continue;
        const std::string key = "cffa_slot" + std::to_string(cffa->getSlot());
        settings->setString(key + "_path",
            cffa->isImageLoaded() ? cffa->getImagePath() : std::string());
        settings->setBool(key + "_writeback", cffa->isWriteBackEnabled());
    }
    // (3.5"/SmartPort media is already saved eagerly on mount.)

    // 1. Stop the worker thread so card destructors don't race against a
    //    running CPU step.
    controller->stop();

    // 2. Tear down all cards and clear our raw pointers. Holding the
    //    state mutex isn't strictly necessary now that the worker is
    //    stopped, but it's cheap insurance against any UI thread that
    //    might be peeking — AND it serialises with the AI control
    //    server's handlers (which take the same mutex around card
    //    pointer reads). aiServer->detach() must happen under this
    //    lock to safely null disk6_/hdv5_ before slotBus.clear()
    //    destroys their pointees.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        aiServer->detach();
        // Every card that owns an AudioSource (Mockingboard / Phasor /
        // Echo+) must be unregistered from the audio device BEFORE the
        // slot bus destroys it — otherwise the audio thread's next
        // callback dereferences a freed source. Same gotcha mirrored
        // in applyProfile's teardown.
        if (controller->audio().isAvailable()) {
            if (mockingboardCard)
                controller->audio().removeSource(mockingboardCard->audioSource());
            if (phasorCard)
                controller->audio().removeSource(phasorCard->audioSource());
            if (echoPlusCard)
                controller->audio().removeSource(echoPlusCard->audioSource());
        }
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
        printerCard      = nullptr;
        smartPortCard    = nullptr;
        controller->memory().slotBus().clear();
        // Also drop any cached display->setChatMauveCard pointer — the
        // next plug call will set it again.
        display->setChatMauveCard(nullptr);
    }

    // 3. Re-run plugSlotsFromSettings() with the freshly-saved keys.
    plugSlotsFromSettings();

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
}

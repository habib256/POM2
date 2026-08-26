// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MainWindow.h"
#include "MainWindowUiState.h"

// Heavy headers — pulled here so MainWindow.h can stay forward-declared.
// Touch any of these only recompiles the MainWindow_*.cpp TUs, not every
// file that includes MainWindow.h.
#include "AiControlServer.h"
#include "AudioCoordinator.h"
#include "DebugCoordinator.h"
#include "NetworkCoordinator.h"
#include "SlotProvisioningCoordinator.h"
#include "SlotConfigurationCoordinator.h"
#include "StorageCoordinator.h"
#include "AtomicFileReplace.h"
#include "Apple2Display.h"
#include "Version.h"
#include "CassetteDeck_ImGui.h"
#include "Rewind_ImGui.h"
#include "CassetteDevice.h"
#include "CharRomCatalog.h"
#include "ClockCard.h"
#include "SoftCardZ80.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "GrapplerCard.h"
#include "NetworkBackend.h"
#include "SlirpNetworkBackend.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"
#include "W5100HostSockets.h"
#include "FujiNetCard.h"
#include "FujiNetHost.h"
#include "Uthernet_ImGui.h"
#include "ImageWriter.h"
#include "ImageWriter_ImGui.h"
#include "RomStatus_ImGui.h"
#include "AbstractionLevels_ImGui.h"
#include "Keyboard_ImGui.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "PrinterFeedCursor.h"
#include "Disk35Controller_ImGui.h"
#include "DiskController_ImGui.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "DiskLibrary_ImGui.h"
#include "EmulationController.h"
#include "HdvController_ImGui.h"
#include "JoystickInput.h"
#include "JoystickPanel_ImGui.h"
#include "LeChatMauveCard.h"
#include "LeChatMauve_ImGui.h"
#include "Logger.h"
#include "MemoryViewer_ImGui.h"
#include "HgrPaintEditor.h"     // portable hgrpaint/ editor (shared with POM1)
#include "HgrSpriteEditor.h"    // portable hgrsprite/ editor (same host seam)
#include "Pom2HgrPaintHost.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "MouseGrab.h"
#include "NtscPostProcessor.h"
#include "CrtEffectStack.h"
#include "Voxel3DRenderer.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "Settings.h"
#include "IconsFontAwesome6.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "FloppyEmuDevice.h"
#include "FloppyEmu_ImGui.h"
#include "PrinterScreenDump.h"
#include "PrinterHistory.h"
#include "PrinterSoundDevice.h"
#include "SmartPort_ImGui.h"
#include "FujiNet_ImGui.h"
#include "SpeakerDevice.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"
#include "SpOverSlipLink.h"
#include "SystemProfile.h"
#include "Toolbar_ImGui.h"
#include "CommandPalette_ImGui.h"

#include "imgui.h"
#include "imgui_internal.h"   // BeginViewportSideBar (status bar)
#include "Pom2GL.h"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <vector>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

// Physical MainWindow split: disk, SmartPort, FujiNet and removable-media panels.

namespace {
// Sentinel prefix used in HdvController_ImGui::LibraryEntry::fullPath to
// flag the synthetic prodos_folder/ host-folder mount. The dispatcher in
// renderHdvPanelWindow detects this prefix and routes to the synthesiser
// instead of treating the path as a real .hdv file.
constexpr const char* kProDOSHostSentinel = "@PRODOS_HOST_FOLDER@:";

bool hdvRequiresSmartPort(pom2::SystemProfile profile)
{
    return profile == pom2::SystemProfile::AppleIIc ||
           profile == pom2::SystemProfile::AppleIIcPlus ||
           profile == pom2::SystemProfile::AppleIIcPAL;
}
} // namespace

void MainWindow::updateAutoTurbo()
{
    const auto diskCards = diskIICards();
    // Auto-turbo: while a disk is actively streaming, crank the CPU to ~60x
    // emulated so loads don't crawl at 1 MHz. Two activity sources:
    //
    //   • DiskII (5.25"): the motor line. Multi-instance friendly — one card
    //     spinning up is enough to enter turbo; all must be idle to leave it.
    //   • ProDOS hard disk (ProDOSHardDiskCard): no motor line, so the byte-
    //     loop firmware streams blocks at the plain CPU rate and HD games
    //     (e.g. Nox Archaist) load far slower than a 5.25" game that gets the
    //     motor-on turbo. Treat recent HDV data-port activity as the same
    //     "busy" signal; the card decays it over a few frames so a multi-block
    //     load stays in turbo end-to-end, then drops back for gameplay.
    //
    // Called every frame from render() (NOT from renderDiskPanelWindow, which
    // early-returns when its window is hidden — the default).
    bool anyMotorOn = false;
    for (auto* c : diskCards) {
        if (c && c->isMotorOn()) { anyMotorOn = true; break; }
    }
    // Decay + poll EVERY block card (HDV + CFFA can coexist) so a load on
    // either keeps turbo engaged. tickActivityDecay() must run on each card
    // (not short-circuit) so their independent decay counters all advance.
    bool hdvBusy = false;
    const auto blocks = blockCards();
    for (auto* dev : blocks) {
        dev->tickActivityDecay();
        if (dev->isBusy()) hdvBusy = true;
    }
    // SmartPort units carry the same hysteretic counter but are NOT
    // ProDOSBlockCards, so `blockCards()` never sees them. Until this loop
    // existed their counters were bumped and never bled off, and — more
    // than a stuck LED — SmartPort media sat outside disk turbo entirely.
    // That is the //c / //c+ boot path for 3.5" and HDV, so the machines
    // most dependent on it were the ones loading at 1 MHz while an HDV card
    // in a //e got ~60×.
    const auto smartPorts = smartPortCards();
    for (auto* sp : smartPorts) {
        for (size_t u = 0; u < pom2::SmartPortCard::kMaxUnits; ++u) {
            if (auto* unit = sp->unit(u)) {
                unit->tickActivityDecay();
                if (unit->isBusy()) hdvBusy = true;
            }
        }
    }
    const bool anyBusy       = anyMotorOn || hdvBusy;
    const bool turboEligible =
        diskTurboWhileMotor &&
        (!diskCards.empty() || !blocks.empty() || !smartPorts.empty());
    if (turboEligible) {
        if (anyBusy && !diskTurboActive) {
            diskSavedCyclesPerFrame = controller->getCyclesPerFrame();
            controller->setCyclesPerFrame(1'000'000);
            diskTurboActive = true;
        } else if (!anyBusy && diskTurboActive) {
            controller->setCyclesPerFrame(diskSavedCyclesPerFrame);
            diskTurboActive = false;
        }
    } else if (diskTurboActive) {
        controller->setCyclesPerFrame(diskSavedCyclesPerFrame);
        diskTurboActive = false;
    }
}

void MainWindow::renderDiskPanelWindow()
{
    if (!uiState_->showDiskPanel) return;
    const auto diskCards = diskIICards();

    // Disk library is the same for every plugged DiskII (it's the
    // contents of disks_5.4/ on disk). Build it once and share via copy.
    std::vector<pom2::DiskController_ImGui::LibraryEntry> sharedLibrary;
    // Disk library — scan disks_5.4/ recursively for .dsk/.do/.po/.nib/.woz.
    // Sub-folders are honoured so users can shelve their library by
    // format (`disks_5.4/dsk/`, `disks_5.4/woz/`, …) or by collection
    // (`disks_5.4/games/`, `disks_5.4/dev/`, …) without losing the one-click
    // boot. Cheap (a few dirent reads per frame); sorted alphabetically
    // so the list doesn't reshuffle as the OS hands us a different
    // dirent order. `displayName` carries the path relative to the
    // scanned root so two files of the same name in different sub-
    // folders don't collide visually.
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "disks_5.4", "../disks_5.4", "../../disks_5.4" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                // Skip dotfiles AND dotdirs (.git, .DS_Store, …). On a
                // dotdir we disable_recursion_pending so we don't walk
                // into it on the next ++.
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                    ext != ".nib" && ext != ".woz") continue;
                pom2::DiskController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                sharedLibrary.push_back(std::move(e));
            }
            break;     // first existing candidate dir wins
        }
        std::sort(sharedLibrary.begin(), sharedLibrary.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    // (Auto-turbo lives in updateAutoTurbo(), called every frame from
    // render() — it must run even when this panel window is hidden, which is
    // the default. See MainWindow::updateAutoTurbo.)

    // ── Render one window per plugged DiskII ──────────────────────────
    // Title carries the slot number so ImGui assigns each card its own
    // window state (position, size, dock). The primary (lowest-slot) card
    // gets the curated default position; subsequent cards cascade slightly
    // down/right so they don't perfectly overlap on first show.
    for (size_t idx = 0; idx < diskCards.size() && idx < diskPanels.size(); ++idx) {
        DiskIICard*                       card  = diskCards[idx];
        pom2::DiskController_ImGui*       panel = diskPanels[idx].get();
        if (!card || !panel) continue;

        pom2::DiskController_ImGui::DriveSnapshot snap;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            snap.bootRomLoaded     = card->hasBootRom();
            snap.diskLoaded        = card->isDiskLoaded();
            snap.motorOn           = card->isMotorOn();
            snap.track             = card->getCurrentTrack();
            snap.halfTrack         = card->getHalfTrack();
            snap.trackPos          = card->getTrackPosition();
            snap.diskPath          = card->getDiskPath();
            snap.lastError         = card->getLastError();
            snap.writeBackEnabled  = card->isWriteBackEnabled();
            snap.hasUnsavedChanges = card->hasUnsavedChanges();
            snap.fileWriteProtected = card->isFileWriteProtected();
        }
        snap.turboWhileMotor = diskTurboWhileMotor;
        snap.turboActive     = diskTurboActive;
        snap.library         = sharedLibrary;     // shared copy

        const float baseX = 1055.0f, baseY = 30.0f;
        ImGui::SetNextWindowPos (
            ImVec2(baseX - static_cast<float>(idx) * 30.0f,
                   baseY + static_cast<float>(idx) * 30.0f),
            ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(705, 960), ImGuiCond_FirstUseEver);

        char title[64];
        std::snprintf(title, sizeof(title),
                      "Disk II (slot %d)", card->getSlot());
        // Only the primary card honours `uiState_->showDiskPanel` (the menu toggle).
        // Secondary cards share the same toggle for simplicity — the user
        // sees them appear/disappear together. We feed the same flag to
        // each render() call.
        auto result = panel->render(title, uiState_->showDiskPanel, snap);

        if (result.turboToggleChanged) {
            diskTurboWhileMotor = result.turboNewValue;
        }
        if (result.writeBackToggleChanged) {
            const auto command = storageCoordinator_->setDiskIIWriteBack(
                *controller, *settings, card->getSlot(),
                result.writeBackNewValue);
            uiState_->tapeStatusMessage = "slot " + std::to_string(card->getSlot()) +
                (command.ok && result.writeBackNewValue
                    ? ": write-back ENABLED (saves on eject)"
                    : command.ok ? ": write-back disabled"
                                 : ": write-back failed: " + command.error);
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
        }
        if (result.requestEject) {
            const auto command = storageCoordinator_->ejectDiskII(
                *controller, *settings, card->getSlot(), 0);
            uiState_->tapeStatusMessage = command.ok
                ? ("Disk ejected (slot " + std::to_string(card->getSlot()) + ")")
                : ("Eject failed: " + command.error);
            uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
        }
        if (result.requestBoot) {
            auto st = controller->lockState();
            card->seekTrack0();
            const uint16_t pc = static_cast<uint16_t>(
                0xC000 + (card->getSlot() << 8));
            st.cpu().setProgramCounter(pc);
            controller->setMode(EmulationController::Mode::Running);
            char msg[64];
            std::snprintf(msg, sizeof(msg), "Boot: PC → $%04X", pc);
            uiState_->tapeStatusMessage = msg;
            uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
        }
        if (!result.requestInsertAndBoot.empty()) {
            const std::string path = result.requestInsertAndBoot;
            const auto command = storageCoordinator_->mountDiskII(
                *controller, *settings, card->getSlot(), 0, path, true);
            if (command.ok) {
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library click → insert + boot: " + path);
                uiState_->tapeStatusMessage = "Booting: " + path;
            } else {
                uiState_->tapeStatusMessage = "Boot failed: " + command.error;
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
        }
        if (!result.requestInsertOnly.empty()) {
            const std::string path = result.requestInsertOnly;
            const auto command = storageCoordinator_->mountDiskII(
                *controller, *settings, card->getSlot(), 0, path);
            if (command.ok) {
                pom2::log().info("Disk II",
                    "slot " + std::to_string(card->getSlot()) +
                    " Library right-click → insert only: " + path);
                uiState_->tapeStatusMessage = "Inserted (no boot): " + path;
            } else {
                uiState_->tapeStatusMessage =
                    "Insert failed: " + command.error;
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
        }
    }
}

// ─── Disk Library (unified browser: 5.25 / 3.5 / HDV) ───────────────────

bool MainWindow::ejectMediaBay(int slot, int index, bool diskII)
{
    // Addressed by SLOT + bay rather than by a card pointer: the status bar
    // builds its rows from a value snapshot taken under the lock and released
    // before drawing, so by the time the user clicks, any pointer captured
    // back then could belong to a card a Slot-Config Apply has since deleted.
    // Re-resolving through the bus here is what makes the click safe.
    std::string label;
    const auto command = diskII
        ? storageCoordinator_->ejectDiskII(
              *controller, *settings, slot, index)
        : storageCoordinator_->ejectMediaBay(
              *controller, *settings, slot, index);
    const bool ok = command.ok;
    label = "slot " + std::to_string(slot);
    if (diskII)
        label += " drive " + std::to_string(index + 1);
    else if (index > 0)
        label += " bay " + std::to_string(index + 1);

    uiState_->tapeStatusMessage = ok ? ("Ejected " + label)
                           : ("Eject refused for " + label + ": " +
                              command.error + " — it stays mounted");
    uiState_->tapeStatusUntil = uiState_->lastFrameTime + (ok ? 3.0 : 8.0);
    if (!ok) pom2::log().warn("Media", uiState_->tapeStatusMessage);
    return ok;
}

void MainWindow::ejectAllDisks()
{
    // The coordinator handles every slot-owned medium plus the on-board
    // 3.5" pair, respecting each device's lock boundary.
    const auto result = storageCoordinator_->ejectAllMedia(
        *controller, *settings);
    for (const auto& failure : result.failures)
        pom2::log().warn("Media", failure);
    uiState_->tapeStatusMessage = result.ok()
        ? "Eject completed"
        : "Eject completed (failed media remain mounted)";
    uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
}

pom2::ProDOSBlockCard* MainWindow::hdvDevice() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).preferredBlock();
}

// Bus *topology* reads (which slot holds which card) are UI-thread-confined:
// every writer — plugSlotsFromSettings, applyProfile, the slot-config
// rebuild — runs on this thread, and the worker only ever reads the table.
// A lock here would protect nothing while reading as though it did; per-card
// *state* is a different matter and does go through lockState().
std::vector<DiskIICard*> MainWindow::diskIICards() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).diskIICards;
}

DiskIICard* MainWindow::primaryDiskIICard() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).primaryDiskII;
}

ProDOSHardDiskCard* MainWindow::primaryHdvCard() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).primaryHdv;
}

pom2::SmartPortCard* MainWindow::primarySmartPortCard() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).primarySmartPort;
}

std::vector<pom2::ProDOSBlockCard*> MainWindow::blockCards() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).blockCards;
}

std::vector<pom2::SmartPortCard*> MainWindow::smartPortCards() const
{
    return storageCoordinator_->topology(
        controller->memory().slotBus()).smartPortCards;
}

bool MainWindow::flushSlotMedia(std::string& err)
{
    std::lock_guard<std::mutex> lk(controller->stateMutex());
    return storageCoordinator_->flushAll(controller->memory().slotBus(), err);
}

bool MainWindow::insertAndBootImage(const std::string& path, std::string& errOut)
{
    const auto diskCards = diskIICards();
    DiskIICard* const diskCard = primaryDiskIICard();
    auto* smartPortCard = primarySmartPortCard();
    // Classify by extension + size, route into the matching slot under the
    // active profile/slot config, then cold-boot. Shared by the CLI
    // positional-disk / --kiosk launcher and (potentially) any future
    // single-call boot entry point. Mirrors the Disk Library "insert +
    // boot" buttons but with no UI surface.
    //
    // No ROM means no Monitor and no Applesoft: the boot PROM would still
    // pull sector 0 and jump into a loader that calls firmware entry points
    // backed by nothing, hanging on a garbage screen. Say so instead of
    // mounting and then reporting a boot that cannot happen.
    if (!romLoaded_) {
        errOut = "no Apple II ROM loaded — see Help > Welcome / Quick Start";
        return false;
    }
    switch (classifyDiskForSlot(path)) {
        case DiskSlotClass::Floppy525: {
            // Prefer the Disk II in the conventional boot slot 6; fall back
            // to the primary (lowest-slot) card. Booting a single floppy
            // from a non-6 slot is unconventional and breaks software that
            // hardcodes slot 6 for its loader — matters when the config has
            // Disk II in several slots (primary = lowest = e.g. slot 5).
            DiskIICard* target = nullptr;
            for (auto* c : diskCards) if (c && c->getSlot() == 6) { target = c; break; }
            if (!target) target = diskCard;
            if (!target) { errOut = "no Disk II card in the current config"; return false; }
            const int targetSlot = target->getSlot();
            const auto command = storageCoordinator_->mountDiskII(
                *controller, *settings, targetSlot, 0, path, true);
            if (!command.ok) {
                errOut = command.error;
                return false;
            }
            if (!controller->bootFromSlot(targetSlot)) {
                errOut = "slot " + std::to_string(targetSlot) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Sony35: {
            // Without a SmartPort card, the storage coordinator falls
            // through to the on-board Sony pair. That pair is guest-visible
            // only on the //c+, so other profiles need a session-local card.
            //
            // When neither exists, auto-plug a Liron-class SmartPort the
            // same way the HDV branch below auto-plugs a block card: a
            // dropped 800K .po/.2mg is explicit "boot this" intent, and
            // failing it on the stock II+/IIe config (which ships no
            // SmartPort) made drag-and-drop refuse the single most common
            // 3.5" distribution format. Session-local, never persisted.
            if (!smartPortCard &&
                activeProfile != pom2::SystemProfile::AppleIIcPlus) {
                const auto provision =
                    slotProvisioningCoordinator_->ensureSmartPortBootTarget(
                        *controller, activeProfile);
                if (!provision) {
                    errOut = provision.error;
                    return false;
                }
            }
            const auto command = storageCoordinator_->mountDisk35(
                *controller, *settings, 0, path);
            if (!command.ok) {
                errOut = command.error;
                return false;
            }
            // SmartPort card present (incl. //c-class built-in slot 5) →
            // boot it explicitly; otherwise cold-boot (//c+ on-board hub).
            if (command.bootSlot >= 0) {
                if (!controller->bootFromSlot(command.bootSlot)) {
                    errOut = "slot " + std::to_string(command.bootSlot) +
                             " did not boot the image (cold-booted instead)";
                    return false;
                }
            } else {
                // //c+ on-board Sony hub. The IWM bit-shift state machine is
                // deliberately unmodelled (CLAUDE.md), so this cold boot does
                // NOT reach the mounted 3.5" disk — the image is mounted and
                // the machine restarted, nothing more. Don't call it a boot.
                controller->coldBoot();
                controller->setMode(EmulationController::Mode::Running);
                errOut = "mounted on the //c+ on-board 3.5\" drive, which "
                         "POM2 cannot boot from (unmodelled IWM) — use a "
                         "SmartPort 3.5\" card to boot this image";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Hdv: {
            // Make sure a card exists to host the HDV (auto-plug one if the
            // saved config has none), then route + boot.
            const auto provision =
                slotProvisioningCoordinator_->ensureHdvBootTarget(
                    *controller, *settings, activeProfile);
            if (!provision) {
                errOut = provision.error;
                return false;
            }
            const auto command = storageCoordinator_->mountHdv(
                *controller, *settings, path,
                hdvRequiresSmartPort(activeProfile));
            if (!command.ok) {
                errOut = command.error;
                return false;
            }
            if (!controller->bootFromSlot(command.bootSlot)) {
                errOut = "slot " + std::to_string(command.bootSlot) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Unknown:
        default:
            errOut = "unrecognised disk image (extension/size): " + path;
            return false;
    }
}

void MainWindow::renderDiskLibraryWindow()
{
    if (!uiState_->showDiskLibrary) return;
    const auto diskCards = diskIICards();
    DiskIICard* const diskCard = primaryDiskIICard();
    auto* const smartPortCard = primarySmartPortCard();

    // Default position: right column of the curated 1568×850 layout,
    // flush against the screen window. 435 px wide × 745 px tall =
    // enough for the 3-tab table + the search/sort header without
    // scroll overflow on a typical 800+ disk library.
    ImGui::SetNextWindowPos (ImVec2(1125, 90),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(435,  745), ImGuiCond_FirstUseEver);

    pom2::DiskLibrary_ImGui::CurrentlyMounted mounted;
    const auto disk35Snapshot =
        storageCoordinator_->captureDisk35(*controller);
    if (disk35Snapshot.drives[0].loaded)
        mounted.disk35Internal = disk35Snapshot.drives[0].path;
    if (disk35Snapshot.drives[1].loaded)
        mounted.disk35External = disk35Snapshot.drives[1].path;
    // Build the mount snapshot under stateMutex, copying every path BY
    // VALUE — same rule as renderDiskPanelWindow / renderSmartPortPanelWindow
    // / renderFloppyEmuWindow. getDiskPath() & friends return a reference
    // into the card's live DiskImage, and the AI control server's HTTP
    // thread reassigns it from /disk insert + eject. The lock is released
    // before diskLibrary->render() below: that path re-locks.
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        // Currently-inserted Disk II images (per plugged card). The library
        // tags rows with `* ` when their path matches any of these — gives
        // the user a visual cue across all plugged cards at once.
        for (auto* c : diskCards) {
            if (!c) continue;
            pom2::DiskLibrary_ImGui::CurrentlyMounted::DiskIICardInfo info;
            info.slot = c->getSlot();
            if (c->isDiskLoaded(0)) {
                info.drive1 = c->getDiskPath(0);
                mounted.diskII.push_back(info.drive1);
            }
            if (c->isDiskLoaded(1)) {
                info.drive2 = c->getDiskPath(1);
                mounted.diskII.push_back(info.drive2);
            }
            mounted.diskIICards.push_back(info);
        }
        if (pom2::ProDOSBlockCard* dev = hdvDevice(); dev && dev->isImageLoaded()) {
            mounted.hdv = dev->getImagePath();
        } else if (smartPortCard) {
            // SmartPort-routed HDV — show as mounted in the Library so the
            // `* ` marker matches reality regardless of which path holds it.
            const pom2::SmartPortUnit* u = smartPortCard->unit(0);
            if (u && u->isLoaded() &&
                u->kindKey() == pom2::SmartPortHdvUnit::kKindKey) {
                mounted.hdv = u->path();
            }
        }
    }

    // Favourites + recents are host state (persisted to state.cfg); the panel
    // only renders them and reports a toggle.
    pom2::DiskLibrary_ImGui::Lists lists;
    lists.favourites   = libraryFavourites_;
    lists.recents      = libraryRecents_;
    lists.hideSizeDate = libraryHideSizeDate_;

    const auto r = diskLibrary->render("Disk Library", uiState_->showDiskLibrary,
                                       mounted, lists);

    if (r.toggleHideSizeDate) libraryHideSizeDate_ = !libraryHideSizeDate_;

    if (!r.toggleFavourite.empty()) {
        auto it = std::find(libraryFavourites_.begin(),
                            libraryFavourites_.end(), r.toggleFavourite);
        if (it != libraryFavourites_.end()) libraryFavourites_.erase(it);
        else libraryFavourites_.push_back(r.toggleFavourite);
    }

    // Anything the user actually mounted this frame becomes the newest recent.
    // Driven off the panel's requests rather than off the cards, so a mount
    // that came from the CLI or a drag-and-drop doesn't silently reorder the
    // list behind the user's back.
    for (const std::string* p : { &r.request525InsertAndBoot,
                                  &r.request525InsertOnly,
                                  &r.request35MountAndBoot,
                                  &r.request35MountOnly,
                                  &r.requestHdvMountAndBoot,
                                  &r.requestHdvMountOnly,
                                  &r.requestFloppyEmuMountAndBoot,
                                  &r.requestFloppyEmuMountOnly }) {
        if (p->empty()) continue;
        noteLibraryRecent(*p);
    }

    // ── Eject-all (header-row button, moved here from the toolbar) ─────
    if (r.requestEjectAllDisks) ejectAllDisks();

    // ── 5.25" actions → the DiskII card/drive the right-click menu picked ──
    // request525Slot = -1 means "primary card" (left-click default); a real
    // slot routes to that specific DiskII card. drive 0 = drive 1, 1 = drive 2.
    auto resolve525 = [&](int slot) -> DiskIICard* {
        if (slot < 0) return diskCard;
        for (auto* c : diskCards) if (c && c->getSlot() == slot) return c;
        return diskCard;
    };
    if (!r.request525InsertAndBoot.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        const std::string path = r.request525InsertAndBoot;
        const int targetSlot = target ? target->getSlot() : -1;
        const auto command = target
            ? storageCoordinator_->mountDiskII(
                  *controller, *settings, targetSlot, drive, path, true)
            : pom2::StorageCoordinator::MediaCommandResult{
                  false, "no Disk II card"};
        if (command.ok) {
            // Boot the target card's slot (its boot PROM boots drive 1).
            const bool booted = controller->bootFromSlot(targetSlot);
            controller->setMode(EmulationController::Mode::Running);
            uiState_->tapeStatusMessage = std::string("Library: inserted") +
                (booted ? " + booted" : " (slot did not boot — cold-booted)") +
                " (slot " + std::to_string(targetSlot) + " drive " +
                std::to_string(drive + 1) + "): " + path;
        } else {
            uiState_->tapeStatusMessage =
                "Library: boot failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
    if (!r.request525InsertOnly.empty()) {
        DiskIICard* target = resolve525(r.request525Slot);
        const int   drive  = (r.request525Drive == 1) ? 1 : 0;
        const int targetSlot = target ? target->getSlot() : -1;
        const auto command = target
            ? storageCoordinator_->mountDiskII(
                  *controller, *settings, targetSlot, drive,
                  r.request525InsertOnly)
            : pom2::StorageCoordinator::MediaCommandResult{
                  false, "no Disk II card"};
        if (command.ok) {
            uiState_->tapeStatusMessage = "Library: inserted (slot " +
                std::to_string(targetSlot) + " drive " +
                std::to_string(drive + 1) + ", no boot): " +
                r.request525InsertOnly;
        } else {
            uiState_->tapeStatusMessage =
                "Library: insert failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }

    // ── 3.5" actions ─────────────────────────────────────────────────
    // The primary SmartPort card owns 3.5" media whenever one is present
    // (including the //c+ built-in slot); otherwise the coordinator falls
    // back to the on-board Sony pair.
    // The Library click is explicit user intent to mount 3.5" here, so
    // we auto-create a SmartPort35Unit on the target index if the slot
    // is empty or holds a different kind (HDV) — the user can re-pick
    // the type later from the SmartPort Configuration panel.
    // The coordinator shares this routing with the CLI insert+boot path.

    if (!r.request35MountAndBoot.empty()) {
        const auto command = storageCoordinator_->mountDisk35(
            *controller, *settings, r.request35BootDrive,
            r.request35MountAndBoot);
        if (command.ok) {
            // Slot-aware boot: explicit `bootFromSlot(N)` whenever a
            // SmartPort card is plugged — now including the //c-class
            // built-in SmartPort (slot 5). No SmartPort card at all means
            // the //c+ on-board Sony hub, whose IWM boot path POM2
            // deliberately does not model: the cold boot below restarts the
            // machine but never reaches the disk, so don't call it a boot.
            bool booted = false;
            if (command.bootSlot >= 0) {
                booted = controller->bootFromSlot(command.bootSlot);
            } else {
                controller->coldBoot();
            }
            uiState_->tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35BootDrive == 0 ? "1" : "2")
                + (booted ? " booted: " : " mounted (did not boot): ")
                + r.request35MountAndBoot;
        } else {
            uiState_->tapeStatusMessage =
                "Library: 3.5\" boot failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
    if (!r.request35MountOnly.empty()) {
        const auto command = storageCoordinator_->mountDisk35(
            *controller, *settings, r.request35MountDrive,
            r.request35MountOnly);
        if (command.ok) {
            uiState_->tapeStatusMessage = "Library: 3.5\" drive "
                + std::string(r.request35MountDrive == 0 ? "1" : "2")
                + " mounted: " + r.request35MountOnly;
        } else {
            uiState_->tapeStatusMessage =
                "Library: 3.5\" mount failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }

    // ── HDV actions ──────────────────────────────────────────────────
    // Two routing targets: the legacy `ProDOSHardDiskCard` slot (if
    // plugged) OR a SmartPort card's unit 0 (if a SmartPort is plugged
    // but no HDV card is). The Library click is explicit intent to
    // mount HDV; on a SmartPort-only config, auto-create / replace
    // unit 0 with a SmartPortHdvUnit so users don't have to detour
    // through the SmartPort Configuration panel.
    if (!r.requestHdvMountAndBoot.empty()) {
        const std::string path = r.requestHdvMountAndBoot;
        const auto command = storageCoordinator_->mountHdv(
            *controller, *settings, path,
            hdvRequiresSmartPort(activeProfile));
        if (command.ok) {
            const bool booted = controller->bootFromSlot(command.bootSlot);
            uiState_->tapeStatusMessage = "Library: HDV (slot " +
                std::to_string(command.bootSlot) +
                (booted ? ") booted: " : ") mounted, did not boot: ") + path;
        } else {
            uiState_->tapeStatusMessage =
                "Library: HDV mount failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
    if (!r.requestHdvMountOnly.empty()) {
        const auto command = storageCoordinator_->mountHdv(
            *controller, *settings, r.requestHdvMountOnly,
            hdvRequiresSmartPort(activeProfile));
        if (command.ok) {
            uiState_->tapeStatusMessage = "Library: HDV mounted: " + r.requestHdvMountOnly;
        } else {
            uiState_->tapeStatusMessage =
                "Library: HDV mount failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }

    // ── Floppy Emu SD card (floppyemu/) ───────────────────────────────
    // The SD card is not a bay: it holds 5.25", 3.5" and Smartport images
    // side by side, because the device emulates all of them. So a click
    // here is FILE-driven and goes through the same helper the CLI
    // positional disk uses — which also auto-plugs an HDV card when the
    // saved config has none. (The OLED panel stays MODE-driven: that is
    // what a Floppy Emu *is*. See DiskLibrary_ImGui.h.)
    if (!r.requestFloppyEmuMountAndBoot.empty()) {
        const std::string path = r.requestFloppyEmuMountAndBoot;
        std::string err;
        uiState_->tapeStatusMessage = insertAndBootImage(path, err)
            ? ("Library: Floppy Emu booted: " + path)
            : ("Library: Floppy Emu boot failed: " + err);
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
    if (!r.requestFloppyEmuMountOnly.empty()) {
        const std::string path = r.requestFloppyEmuMountOnly;
        std::string err;
        bool ok = false;
        switch (classifyDiskForSlot(path)) {
            case DiskSlotClass::Floppy525:
                if (diskCard) {
                    const auto command = storageCoordinator_->mountDiskII(
                        *controller, *settings, diskCard->getSlot(), 0, path);
                    ok = command.ok;
                    if (!ok) err = command.error;
                } else {
                    err = "no Disk II card in the current config";
                }
                break;
            case DiskSlotClass::Sony35:
            {
                const auto command = storageCoordinator_->mountDisk35(
                    *controller, *settings, 0, path);
                ok = command.ok;
                if (!ok) err = command.error;
                break;
            }
            case DiskSlotClass::Hdv: {
                const auto command = storageCoordinator_->mountHdv(
                    *controller, *settings, path,
                    hdvRequiresSmartPort(activeProfile));
                ok = command.ok;
                if (!ok) err = command.error;
                break;
            }
            case DiskSlotClass::Unknown:
            default:
                err = "unrecognised disk image (extension/size)";
                break;
        }
        uiState_->tapeStatusMessage = ok
            ? ("Library: Floppy Emu mounted: " + path)
            : ("Library: Floppy Emu mount failed: " + err);
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }

    // ── Eject actions ─────────────────────────────────────────────────
    // 5.25": eject from whichever plugged DiskII holds the clicked
    // image. Match by path so multi-instance DiskII setups (the same
    // image plugged into two slots) all clear together.
    if (!r.request525EjectPath.empty()) {
        std::vector<std::pair<int, int>> targets;
        {
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            for (auto* c : diskCards) {
                if (!c) continue;
                for (int d = 0; d < DiskIICard::kDriveCount; ++d) {
                    if (c->isDiskLoaded(d) &&
                        c->getDiskPath(d) == r.request525EjectPath) {
                        targets.emplace_back(c->getSlot(), d);
                    }
                }
            }
        }
        bool ok = true;
        std::string err;
        for (const auto& [slot, drive] : targets) {
            const auto command = storageCoordinator_->ejectDiskII(
                *controller, *settings, slot, drive);
            if (!command.ok) {
                ok = false;
                err = command.error;
            }
        }
        uiState_->tapeStatusMessage = ok ? "Library: 5.25\" disk ejected"
                               : "Library: 5.25\" eject failed: " + err;
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
    }
    if (r.request35EjectDrive >= 0) {
        const auto command = storageCoordinator_->ejectDisk35(
            *controller, *settings, r.request35EjectDrive);
        uiState_->tapeStatusMessage = command.ok
            ? ("Library: 3.5\" drive " +
               std::string(r.request35EjectDrive == 0 ? "1" : "2") + " ejected")
            : ("Library: 3.5\" eject failed: " + command.error);
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
    }
    if (r.requestHdvEject) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            const auto command = storageCoordinator_->ejectMediaBay(
                *controller, *settings, dev->getSlot(), 0);
            uiState_->tapeStatusMessage = command.ok
                ? "Library: HDV ejected"
                : "Library: HDV eject failed: " + command.error;
            uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 3.0;
        }
    }
}

// ─── HDV (slot 5) ────────────────────────────────────────────────────────

void MainWindow::renderSmartPortPanelWindow()
{
    if (!uiState_->showSmartPortPanel) return;

    const auto snap =
        storageCoordinator_->captureSmartPortPanel(*controller);

    char title[64];
    if (snap.plugged) {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration (slot %d)", snap.slot);
    } else {
        std::snprintf(title, sizeof(title),
                      "SmartPort Configuration");
    }

    const auto r = smartPortPanel->render(title, uiState_->showSmartPortPanel, snap);

    if (!snap.plugged) return;
    const auto status = storageCoordinator_->applySmartPortPanel(
        *controller, *settings, snap.slot, r);
    if (!status.message.empty()) {
        uiState_->tapeStatusMessage = status.message;
        uiState_->tapeStatusUntil =
            uiState_->lastFrameTime + status.visibleSeconds;
    }
}

bool MainWindow::plugFujiNetFromCli(int& slot, bool slotExplicit, bool serial,
                                    const std::string& serialDevice,
                                    int tcpPort, std::string& errOut)
{
    if (slot < 1 || slot > 7) { errOut = "slot must be 1-7"; return false; }
    if (pom2::profileConfig(activeProfile).noPhysicalSlots) {
        errOut = "the active //c-class profile has no physical expansion slots";
        return false;
    }

    auto  st  = controller->lockState();
    auto& bus = st.memory().slotBus();

    if (bus.peripheral(slot) != nullptr && !slotExplicit) {
        // The user never named a slot — 7 is only POM2's preference, and its
        // own first-run default puts a Le Chat Mauve there, so refusing here
        // would make the documented bare `--fujinet` fail on a stock install.
        // Fall back the way docs/fujinet_plan.md specifies. Downwards from 7:
        // the autostart scan walks slots high to low, so the highest free slot
        // is the one most likely to be reached before the Disk II in 6.
        int free = 0;
        for (int s = 7; s >= 1 && free == 0; --s)
            if (bus.peripheral(s) == nullptr) free = s;
        if (free == 0) {
            errOut = "every slot is occupied — free one, or name it with "
                     "--fujinet-slot";
            return false;
        }
        pom2::log().info("CLI", "--fujinet: slot " + std::to_string(slot) +
                                    " holds " +
                                    std::string(bus.peripheral(slot)->name()) +
                                    ", using free slot " + std::to_string(free));
        slot = free;
    }

    if (bus.peripheral(slot) != nullptr) {
        errOut = "slot " + std::to_string(slot) + " already holds " +
                 std::string(bus.peripheral(slot)->name()) +
                 " — pick a free slot with --fujinet-slot";
        return false;
    }

    if (!plugFujiNetUnlocked(st, slot, serial, serialDevice, tcpPort, errOut))
        return false;

    // Remember it so every later slot rebuild reproduces it — see the header.
    cliFujiNetSlot_       = slot;
    cliFujiNetSerial_     = serial;
    cliFujiNetSerialPath_ = serialDevice;
    cliFujiNetPort_       = tcpPort;
    return true;
}

bool MainWindow::plugFujiNetUnlocked(const pom2::StateAccess& st,
                                     int slot, bool serial,
                                     const std::string& serialDevice,
                                     int tcpPort, std::string& errOut)
{
    auto& bus = st.memory().slotBus();
    auto card = std::make_unique<pom2::FujiNetCard>(slot);
    auto hostLink = std::make_unique<pom2::SpOverSlipLink>();
    card->setMemory(&st.memory());
    card->setCpu(&st.cpu());
    auto& link = *hostLink;
    if (serial)
        link.setSerialMode(serialDevice, pom2::SerialPort::kDefaultBaud);
    else
        link.setTcpMode(static_cast<uint16_t>(tcpPort));

    std::string err;
    if (!link.start(err)) { errOut = err; return false; }

    card->setLink(std::move(hostLink));
    bus.plug(slot, std::move(card));
    return true;
}

void MainWindow::archiveNewPrinterPages()
{
    if (!imageWriter || !printerHistory || !printerHistory->isOpen()) return;

    const uint64_t ejected = static_cast<uint64_t>(imageWriter->sheetsEjected());
    if (ejected <= uiState_->printerArchivedSheets) return;

    // How many of those sheets are still reachable. The stack is capped, so a
    // burst of form feeds between two frames can push pages off it before we
    // ever see them — archive what is there and resynchronise rather than
    // pretending we captured everything.
    const uint64_t missed = ejected - uiState_->printerArchivedSheets;
    const size_t   have   = imageWriter->completedPageCount();
    const size_t   take   = static_cast<size_t>(std::min<uint64_t>(missed, have));
    const uint64_t irrecoverablyDropped = missed - take;
    size_t accepted = 0;

    for (size_t i = have - take; i < have; ++i) {
        std::string err;
        if (!printerHistory->addPage(imageWriter->completedPage(i),
                                     static_cast<int>(imageWriter->model()),
                                     static_cast<int>(imageWriter->ribbon()),
                                     imageWriter->paperWidthIn(),
                                     imageWriter->paperLengthIn(), err)) {
            pom2::log().warn("PrinterHistory", err);
            break;
        }
        ++accepted;
    }
    if (take < missed) {
        pom2::log().warn("PrinterHistory",
            "archived " + std::to_string(take) + " of " +
            std::to_string(missed) + " ejected sheets — the rest had already "
            "fallen off the printer's page stack");
    }
    // Advance only past sheets actually archived (plus sheets already fallen
    // off the bounded live stack). A transient index failure is retried next
    // frame instead of silently discarding the failed page and the rest of
    // the batch.
    uiState_->printerArchivedSheets += irrecoverablyDropped + accepted;
}

void MainWindow::dumpScreenToPrinter()
{
    if (!imageWriter) return;

    // Snapshot the framebuffer under BOTH locks, exactly as saveScreenshot
    // does — `pixels()` can lazily run the composite demod, which writes
    // frame80 and races the AI control server's /screen handler otherwise.
    // Lock order stateMutex → demodMutex, never the other way.
    int w = 0, h = 0;
    std::vector<uint32_t> px;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        std::lock_guard<std::mutex> demodLk(display->demodMutex());
        w = display->width();
        h = display->height();
        if (w > 0 && h > 0) {
            const uint32_t* src = display->pixels();
            px.assign(src, src + static_cast<size_t>(w) * h);
        }
    }
    if (px.empty()) return;

    // The dump goes in as BYTES, through the printer's own ESC G parser —
    // never painted onto the page. So it obeys the ribbon, the pacing and the
    // paper, lands in the tray and the PDF export, and cannot drift from what
    // a period driver would have produced. See PrinterScreenDump.h.
    // Each head has its own graphics grammar, so the dump has to be built
    // for the one actually fitted — the Epson wants ESC * with a binary count
    // and bit 7 as the top dot, the C. Itoh family ESC G with four ASCII
    // digits and bit 0.
    std::vector<uint8_t> stream;
    if (imageWriter->model() == pom2::IwModel::EpsonFX80)
        pom2::buildScreenDumpEpson(px.data(), w, h, w,
                                   uiState_->printerDumpOptions, stream);
    else
        pom2::buildScreenDumpImageWriter(px.data(), w, h, w,
                                         uiState_->printerDumpOptions, stream);
    if (stream.empty()) return;

    imageWriter->queueBytes(stream.data(), stream.size());
    uiState_->showImageWriterPanel = true;  // the user asked to print; show the paper
}

void MainWindow::renderFujiNetPanelWindow()
{
    if (!uiState_->showFujiNetPanel) return;

    const auto snap = networkCoordinator_->captureFujiNetPanel(*controller);

    const auto r = fujiNetPanel->render("FujiNet", uiState_->showFujiNetPanel, snap);
    if (!snap.plugged) return;
    networkCoordinator_->applyFujiNetPanel(*controller, r);
}

void MainWindow::renderFloppyEmuWindow()
{
    if (!uiState_->showFloppyEmu) return;
    DiskIICard* const diskCard = primaryDiskIICard();
    auto* smartPortCard = primarySmartPortCard();
    namespace fs = std::filesystem;
    using Mode = pom2::FloppyEmuMode;
    const Mode mode = floppyEmu->mode();

    auto baseName = [](const std::string& p) {
        return fs::path(p).filename().string();
    };
    auto human = [](uint64_t b) -> std::string {
        if (b == 0)              return std::string();
        if (b >= 1024 * 1024)    return std::to_string(b / (1024 * 1024)) + "M";
        if (b >= 1024)           return std::to_string(b / 1024) + "K";
        return std::to_string(b) + "B";
    };
    auto controllerReady = [&](Mode m) -> bool {
        switch (m) {
            case Mode::Disk525:   return diskCard != nullptr;
            case Mode::Disk35:
            case Mode::Unidisk35: return smartPortCard != nullptr ||
                                         activeProfile == pom2::SystemProfile::AppleIIcPlus;
            case Mode::SmartportHD: return hdvDevice() != nullptr ||
                                           smartPortCard != nullptr;
        }
        return false;
    };
    auto controllerHint = [&](Mode m) -> std::string {
        switch (m) {
            case Mode::Disk525:
                return "No Disk II controller — add 'Disk II' in the Slot Manager.";
            case Mode::Disk35:
            case Mode::Unidisk35:
                return "No SmartPort/Liron controller for 3.5\" media.";
            case Mode::SmartportHD:
                return "No SmartPort or HDV controller for hard-disk media.";
        }
        return std::string();
    };
    auto insertedLabel = [&](Mode m) -> std::string {
        // Snapshot-under-lock: load state / paths are worker-mutable
        // (inserts can come from soft-switch-triggered write-back paths
        // and other panels). Lock released before any rendering.
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        switch (m) {
            case Mode::Disk525:
                return (diskCard && diskCard->isDiskLoaded())
                           ? baseName(diskCard->getDiskPath()) : std::string();
            case Mode::Disk35:
            case Mode::Unidisk35:
                if (smartPortCard) {
                    const pom2::SmartPortUnit* u = smartPortCard->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return controller->disk35Internal().isLoaded()
                           ? baseName(controller->disk35Internal().path())
                           : std::string();
            case Mode::SmartportHD:
                if (pom2::ProDOSBlockCard* dev = hdvDevice())
                    return dev->isImageLoaded() ? baseName(dev->getImagePath())
                                                : std::string();
                if (smartPortCard) {
                    const pom2::SmartPortUnit* u = smartPortCard->unit(0);
                    return (u && u->isLoaded()) ? baseName(u->path()) : std::string();
                }
                return std::string();
        }
        return std::string();
    };
    auto mountImage = [&](const std::string& path, Mode m) {
        // Selecting an image BOOTS it, like a left-click in the Disk Library
        // ("left-click = insert + boot"). Mounting alone left the user
        // staring at whatever was already on screen with a status line
        // telling them to go and reboot the machine themselves — the image
        // was in the drive and nothing had happened, which reads as "the
        // click did nothing". Routing stays MODE-driven, not
        // extension-driven: the Floppy Emu emulates the device its mode
        // says, so a .2mg selected in Smartport mode must boot from the
        // SmartPort slot even though insertAndBootImage would classify the
        // same file by its extension. -1 = mount succeeded but no explicit
        // boot slot (cold-boot instead); -2 = nothing mounted.
        constexpr int kNoMount   = -2;
        constexpr int kColdBoot  = -1;
        int bootTarget = kNoMount;
        switch (m) {
            case Mode::Disk525: {
                if (!diskCard) { uiState_->floppyEmuStatus = controllerHint(m); break; }
                const int slot = diskCard->getSlot();
                const auto command = storageCoordinator_->mountDiskII(
                    *controller, *settings, slot, 0, path, true);
                if (command.ok) bootTarget = slot;
                uiState_->floppyEmuStatus = command.ok
                    ? ("Booting " + baseName(path))
                    : ("5.25 mount failed: " + command.error);
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35: {
                if (!controllerReady(m)) {
                    const auto provision =
                        slotProvisioningCoordinator_->ensureSmartPortBootTarget(
                            *controller, activeProfile);
                    if (!provision) {
                        uiState_->floppyEmuStatus = provision.error;
                        break;
                    }
                }
                {
                    const auto command = storageCoordinator_->mountDisk35(
                        *controller, *settings, 0, path);
                    if (command.ok) {
                        bootTarget = command.bootSlot >= 0
                            ? command.bootSlot : kColdBoot;
                        uiState_->floppyEmuStatus = "Booting " + baseName(path);
                    } else {
                        uiState_->floppyEmuStatus =
                            "3.5\" mount failed: " + command.error;
                    }
                }
                break;
            }
            case Mode::SmartportHD: {
                if (!controllerReady(m)) {
                    const auto provision =
                        slotProvisioningCoordinator_->ensureSmartPortBootTarget(
                            *controller, activeProfile);
                    if (!provision) {
                        uiState_->floppyEmuStatus = provision.error;
                        break;
                    }
                }
                {
                    const auto command = storageCoordinator_->mountHdv(
                        *controller, *settings, path,
                        hdvRequiresSmartPort(activeProfile));
                    if (command.ok) {
                        bootTarget = command.bootSlot;
                        uiState_->floppyEmuStatus = "Booting " + baseName(path);
                    } else {
                        uiState_->floppyEmuStatus =
                            "Smartport mount failed: " + command.error;
                    }
                }
                break;
            }
        }
        if (bootTarget != kNoMount) {
            if (bootTarget == kColdBoot) controller->coldBoot();
            else                        controller->bootFromSlot(bootTarget);
            controller->setMode(EmulationController::Mode::Running);
        }
    };
    auto ejectCurrent = [&](Mode m) {
        bool ok = false;
        std::string err;
        switch (m) {
            case Mode::Disk525: {
                if (diskCard) {
                    const auto command = storageCoordinator_->ejectDiskII(
                        *controller, *settings, diskCard->getSlot(), 0);
                    ok = command.ok;
                    if (!ok) err = command.error;
                }
                break;
            }
            case Mode::Disk35:
            case Mode::Unidisk35:
            {
                const auto command = storageCoordinator_->ejectDisk35(
                    *controller, *settings, 0);
                ok = command.ok;
                if (!ok) err = command.error;
                break;
            }
            case Mode::SmartportHD: {
                if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
                    const auto command = storageCoordinator_->ejectMediaBay(
                        *controller, *settings, dev->getSlot(), 0);
                    ok = command.ok;
                    if (!ok) err = command.error;
                }
                else if (smartPortCard) {
                    const auto command = storageCoordinator_->ejectMediaBay(
                        *controller, *settings, smartPortCard->getSlot(), 0);
                    ok = command.ok;
                    if (!ok) err = command.error;
                }
                break;
            }
        }
        uiState_->floppyEmuStatus = ok ? "Ejected" : "Eject failed: " + err;
    };

    // ── Build the snapshot. ──────────────────────────────────────────────
    pom2::FloppyEmu_ImGui::Snapshot snap;
    snap.modeLabel = pom2::FloppyEmuDevice::modeLabel(mode);
    snap.sdPresent = floppyEmu->sdPresent();
    snap.sdRootDisplay = floppyEmu->sdRoot();
    {
        const std::string cur = floppyEmu->currentDir();
        const std::string root = floppyEmu->sdRoot();
        snap.dirLabel = (cur.size() >= root.size() &&
                         cur.compare(0, root.size(), root) == 0)
                            ? cur.substr(root.size()) : cur;
    }
    const auto fav = floppyEmu->favorites();
    snap.favoritesAvailable = fav.present;
    snap.favoritesActive    = uiState_->floppyEmuFavoritesActive && fav.present;
    if (snap.favoritesActive) {
        for (const auto& e : fav.entries) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.sublabel = human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    } else {
        for (const auto& e : floppyEmu->listing()) {
            pom2::FloppyEmu_ImGui::Item it;
            it.label = e.name; it.isDir = e.isDir; it.isUp = e.isUp;
            it.sublabel = e.isDir ? "DIR" : human(e.sizeBytes);
            snap.items.push_back(std::move(it));
        }
    }
    snap.controllerReady = controllerReady(mode);
    snap.controllerHint  = controllerHint(mode);
    snap.insertedLabel   = insertedLabel(mode);
    snap.statusLine      = uiState_->floppyEmuStatus;
    for (Mode m : pom2::FloppyEmuDevice::allModes()) {
        snap.modeOptions.push_back(pom2::FloppyEmuDevice::modeLabel(m));
        if (m == mode) snap.currentModeIndex =
            static_cast<int>(snap.modeOptions.size()) - 1;
    }

    const auto r = floppyEmuPanel->render("Floppy Emu (BMOW)", uiState_->showFloppyEmu, snap);

    // ── Apply actions. ───────────────────────────────────────────────────
    if (r.setModeIndex >= 0) {
        const auto modes = pom2::FloppyEmuDevice::allModes();
        if (r.setModeIndex < static_cast<int>(modes.size())) {
            floppyEmu->setMode(modes[r.setModeIndex]);
            uiState_->floppyEmuFavoritesActive = false;
            uiState_->floppyEmuStatus = std::string("Mode: ") +
                pom2::FloppyEmuDevice::modeLabel(modes[r.setModeIndex]);
        }
    }
    if (r.toggleFavorites) uiState_->floppyEmuFavoritesActive = !uiState_->floppyEmuFavoritesActive;
    if (r.requestConfigureController) {
        if (mode == Mode::Disk525)
            uiState_->floppyEmuStatus = "Add a Disk II card via the Slot Manager (Apply restarts).";
        else {
            const auto provision =
                slotProvisioningCoordinator_->ensureSmartPortBootTarget(
                    *controller, activeProfile);
            uiState_->floppyEmuStatus = provision
                ? (std::string(provision.added ? "Added" : "Using") +
                   " SmartPort card in slot " +
                   std::to_string(provision.slot))
                : provision.error;
        }
    }
    if (r.requestEject) ejectCurrent(mode);
    if (r.activateIndex >= 0) {
        if (snap.favoritesActive) {
            if (r.activateIndex < static_cast<int>(fav.entries.size()))
                mountImage(fav.entries[r.activateIndex].fullPath, mode);
        } else {
            const auto items = floppyEmu->listing();
            if (r.activateIndex < static_cast<int>(items.size())) {
                const auto& e = items[r.activateIndex];
                if (e.isDir || e.isUp) floppyEmu->enterDir(e);
                else                   mountImage(e.fullPath, mode);
            }
        }
    }
}

void MainWindow::renderHdvPanelWindow()
{
    if (!uiState_->showHdvPanel) return;
    auto* const hdvCard = primaryHdvCard();

    pom2::HdvController_ImGui::DriveSnapshot snap;
    if (hdvCard) {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        snap.imageLoaded       = hdvCard->isImageLoaded();
        snap.imagePath         = hdvCard->getImagePath();
        snap.blockCount        = hdvCard->getBlockCount();
        snap.writeBackEnabled  = hdvCard->isWriteBackEnabled();
        snap.hasUnsavedChanges = hdvCard->hasUnsavedChanges();
        snap.supportsWriteBack = hdvCard->canWriteBack();
        snap.isSynthVolume     = hdvCard->isSynthVolumeMounted();
    }

    // Library scan — hdv/ for .hdv and .2mg, sorted alphabetically so the
    // list stays stable across frames regardless of dirent order. Plus a
    // synthetic entry for prodos_folder/ if that folder exists (host-folder
    // mount: contents are synthesised into a read-only ProDOS volume on
    // click, see kProDOSHostSentinel below).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        const char* dirCandidates[] = { "hdv", "../hdv", "../../hdv" };
        for (const char* dir : dirCandidates) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            // Recursive walk so users can shelve images by collection /
            // OS / target machine (`hdv/prodos/`, `hdv/iigs/`, …) and
            // still get one-click mount. See the Disk II library
            // comment above for the rationale and ignore rules.
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".hdv" && ext != ".2mg") continue;
                pom2::HdvController_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath    = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });

        // Synthetic entry for the host-folder mount.
        const char* prodosCandidates[] = {
            "prodos_folder", "../prodos_folder", "../../prodos_folder"
        };
        std::string prodosDir;
        for (const char* d : prodosCandidates) {
            if (fs::is_directory(d, ec)) { prodosDir = d; break; }
        }
        if (!prodosDir.empty()) {
            std::size_t fileCount = 0;
            for (const auto& e : fs::directory_iterator(prodosDir, ec)) {
                if (e.is_regular_file()) ++fileCount;
            }
            pom2::HdvController_ImGui::LibraryEntry e;
            e.displayName = "[host folder] " + prodosDir + "/  ("
                          + std::to_string(fileCount) + " files)";
            e.fullPath    = std::string(kProDOSHostSentinel) + prodosDir;
            snap.library.push_back(std::move(e));
        }
    }

    // HDV = bottom-left panel in the curated layout (under the Screen).
    ImGui::SetNextWindowPos (ImVec2(5,    600), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(1040, 390), ImGuiCond_FirstUseEver);
    // Title reflects the actual slot the HDV card is plugged in.
    char hdvTitle[48];
    std::snprintf(hdvTitle, sizeof(hdvTitle), "HDV (slot %d)",
                  hdvCard ? hdvCard->getSlot() : 5);
    auto result = hdvPanel->render(hdvTitle, uiState_->showHdvPanel, snap);

    if (result.writeBackToggleChanged && hdvCard) {
        const auto command = storageCoordinator_->setMediaBayWriteBack(
            *controller, *settings, hdvCard->getSlot(), 0,
            result.writeBackNewValue);
        uiState_->tapeStatusMessage = command.ok && result.writeBackNewValue
            ? "HDV: write-back ENABLED (saves on eject)"
            : command.ok ? "HDV: write-back disabled"
                         : "HDV: write-back failed: " + command.error;
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
    }
    if (result.requestEject && hdvCard) {
        const auto command = storageCoordinator_->ejectMediaBay(
            *controller, *settings, hdvCard->getSlot(), 0);
        uiState_->tapeStatusMessage = command.ok
            ? "HDV ejected" : "HDV eject failed: " + command.error;
        uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 4.0;
    }
    if (result.requestBoot && hdvCard) {
        bootHdvImage();
    }
    if (!result.requestMountAndBoot.empty() && hdvCard) {
        const std::string path = result.requestMountAndBoot;
        const std::string sentinel(kProDOSHostSentinel);

        if (path.rfind(sentinel, 0) == 0) {
            // Host-folder mount: synthesise a read-only ProDOS volume from
            // the folder contents and load it into the slot 5 card. We do
            // NOT auto-boot — block 0 is zero, so the volume isn't
            // bootable. The user boots ProDOS from elsewhere (Disk II or
            // an HDV) and ProDOS then sees /HOST/ as a second drive.
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                uiState_->tapeStatusMessage = "ProDOS synth failed: " + br.error;
                uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 5.0;
                return;
            }
            const auto command = storageCoordinator_->mountBlockBytes(
                *controller, *settings, hdvCard->getSlot(), std::move(bytes),
                std::string("[host folder] ") + hostDir, hostDir);
            if (command.ok) {
                char msg[200];
                std::snprintf(msg, sizeof(msg),
                    "/HOST/ mounted from %s (%zu files, %zu skipped, %zu blocks). Boot ProDOS from another drive.",
                    hostDir.c_str(), br.filesIncluded, br.filesSkipped, br.totalBlocks);
                uiState_->tapeStatusMessage = msg;
                pom2::log().info("HDV",
                    std::string("Synthesised volume from ") + hostDir +
                    " (" + std::to_string(br.filesIncluded) + " files, " +
                    std::to_string(br.totalBlocks) + " blocks)");
            } else {
                uiState_->tapeStatusMessage =
                    "Synth load failed: " + command.error;
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 8.0;
            return;
        }

        // Real .hdv / .2mg / .po file: load under the lock so the card
        // has the right blocks before bootFromSlot wipes RAM and jumps
        // PC = $C(N)00 (where N is the slot the card actually lives in).
        // Two-step lock is safe — the CPU worker only resumes when
        // bootFromSlot flips mode to Running.
        const int slot = hdvCard->getSlot();
        const auto command = storageCoordinator_->mountMediaBay(
            *controller, *settings, slot, 0, path);
        if (command.ok) {
            controller->bootFromSlot(slot);
            pom2::log().info("HDV",
                "slot " + std::to_string(slot) +
                " library click → mount + boot: " + path);
            uiState_->tapeStatusMessage = "Mounting + booting HDV (slot " +
                std::to_string(slot) + "): " + path;
        } else {
            uiState_->tapeStatusMessage = "Boot failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
    if (!result.requestMountOnly.empty() && hdvCard) {
        // Right-click "mount only": swap the image without booting.
        // Host-folder sentinel is handled the same as mount-and-boot
        // above (it never auto-boots anyway), so funnel both branches
        // here when no Apple II reset is wanted.
        const std::string path = result.requestMountOnly;
        const std::string sentinel(kProDOSHostSentinel);

        pom2::StorageCoordinator::MediaCommandResult command;
        if (path.rfind(sentinel, 0) == 0) {
            const std::string hostDir = path.substr(sentinel.size());
            std::vector<std::uint8_t> bytes;
            auto br = pom2::buildVolumeFromFolder(hostDir, "HOST", bytes);
            if (!br.ok) {
                uiState_->tapeStatusMessage = "ProDOS synth failed: " + br.error;
                uiState_->tapeStatusUntil   = uiState_->lastFrameTime + 5.0;
                return;
            }
            command = storageCoordinator_->mountBlockBytes(
                *controller, *settings, hdvCard->getSlot(), std::move(bytes),
                std::string("[host folder] ") + hostDir, hostDir);
        } else {
            command = storageCoordinator_->mountMediaBay(
                *controller, *settings, hdvCard->getSlot(), 0, path);
        }
        if (command.ok) {
            pom2::log().info("HDV",
                std::string("Library right-click → mount only: ") + path);
            uiState_->tapeStatusMessage = "Mounted (no boot): " + path;
        } else {
            uiState_->tapeStatusMessage =
                "Mount failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
}

void MainWindow::renderDiskFileDialog()
{
    const auto diskCards = diskIICards();
    DiskIICard* const diskCard = primaryDiskIICard();
    // Find the panel that currently has its insertDialogOpen flag set.
    // With option C (multi-instance DiskII), any of the per-card panels
    // could have triggered the popup via its "Insert .dsk..." button —
    // we route the eventual insertDisk() to the corresponding card.
    pom2::DiskController_ImGui* triggeredPanel = nullptr;
    DiskIICard*                 triggeredCard  = nullptr;
    for (size_t i = 0; i < diskPanels.size() && i < diskCards.size(); ++i) {
        if (diskPanels[i] && diskPanels[i]->insertDialogOpen) {
            triggeredPanel = diskPanels[i].get();
            triggeredCard  = diskCards[i];
            break;
        }
    }
    // Top-level "Insert disk image..." menu (no per-panel context) routes
    // to the primary card by convention.
    if (!triggeredPanel && diskPanel && diskPanel->insertDialogOpen) {
        triggeredPanel = diskPanel;
        triggeredCard  = diskCard;
    }

    if (triggeredPanel) {
        ImGui::OpenPopup("Insert disk image");
        triggeredPanel->insertDialogOpen = false;
        // Remember which card the popup routes to until the user clicks
        // Insert / Cancel. ImGui modal state survives between frames so
        // the pointer needs to survive too.
        uiState_->diskDialogTargetSlot = triggeredCard ? triggeredCard->getSlot() : -1;
    }
    if (!ImGui::BeginPopupModal("Insert disk image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    // Resolve the target card via the saved slot — the panel pointer may
    // have moved (rare profile-switch races), but the slot number is
    // stable until plugSlotsFromSettings rebuilds.
    DiskIICard*                 popupCard  = nullptr;
    pom2::DiskController_ImGui* popupPanel = nullptr;
    for (size_t i = 0; i < diskCards.size(); ++i) {
        if (diskCards[i] && diskCards[i]->getSlot() == uiState_->diskDialogTargetSlot) {
            popupCard  = diskCards[i];
            popupPanel = (i < diskPanels.size()) ? diskPanels[i].get() : nullptr;
            break;
        }
    }
    if (!popupPanel) popupPanel = diskPanel;
    if (!popupCard)  popupCard  = diskCard;

    if (popupCard) {
        ImGui::Text("Target: Disk II slot %d", popupCard->getSlot());
    }
    ImGui::TextUnformatted("Path to a 5.25\" image —"
                           " .dsk / .do (DOS 3.3, 143 360 B) or"
                           " .po (ProDOS, 143 360 B) or .nib (raw"
                           " nibble stream, 232 960 B) or .woz"
                           " (bit-cell, copy-protected disks; read-only)."
                           " Write-back is opt-in via the panel checkbox.");
    if (popupPanel) {
        char buf[512] = {0};
        std::snprintf(buf, sizeof(buf), "%s", popupPanel->dialogPath.c_str());
        if (ImGui::InputText("##DiskPath", buf, sizeof(buf),
                             ImGuiInputTextFlags_EnterReturnsTrue))
            popupPanel->dialogPath = buf;
        else
            popupPanel->dialogPath = buf;
    }

    // Quick list of disk images in disks_5.4/ (mirrors the cassette dialog).
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks_5.4", "../disks_5.4", "../../disks_5.4" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".dsk" && ext != ".do" && ext != ".po" &&
                ext != ".nib" && ext != ".woz") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()) && popupPanel)
                popupPanel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Insert", ImVec2(120, 0))) {
        if (popupCard && popupPanel && !popupPanel->dialogPath.empty()) {
            const auto command = storageCoordinator_->mountDiskII(
                *controller, *settings, popupCard->getSlot(), 0,
                popupPanel->dialogPath);
            if (command.ok) {
                uiState_->tapeStatusMessage = "Disk inserted (slot " +
                    std::to_string(popupCard->getSlot()) + "): " +
                    popupPanel->dialogPath;
            } else {
                uiState_->tapeStatusMessage =
                    "Insert failed: " + command.error;
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
        }
        uiState_->diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        uiState_->diskDialogTargetSlot = -1;   // popup closed — clear the latched slot
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void MainWindow::renderHdvFileDialog()
{
    auto* const hdvCard = primaryHdvCard();
    if (hdvPanel->mountDialogOpen) {
        ImGui::OpenPopup("Mount HDV image");
        hdvPanel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount HDV image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextUnformatted("ProDOS block-device image — .hdv (raw blocks)"
                           " or .2mg (with 2IMG header, ProDOS order)");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", hdvPanel->dialogPath.c_str());
    if (ImGui::InputText("##HdvPath", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        hdvPanel->dialogPath = buf;
    else
        hdvPanel->dialogPath = buf;

    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "hdv", "../hdv", "../../hdv" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".hdv" && ext != ".2mg") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                hdvPanel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    const bool canMount = hdvCard && !hdvPanel->dialogPath.empty();
    ImGui::BeginDisabled(!canMount);
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        const auto command = storageCoordinator_->mountMediaBay(
            *controller, *settings, hdvCard->getSlot(), 0,
            hdvPanel->dialogPath);
        if (command.ok) {
            uiState_->tapeStatusMessage = "HDV mounted: " + hdvPanel->dialogPath;
        } else {
            uiState_->tapeStatusMessage =
                "HDV mount failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Mount and Boot", ImVec2(160, 0))) {
        const auto command = storageCoordinator_->mountMediaBay(
            *controller, *settings, hdvCard->getSlot(), 0,
            hdvPanel->dialogPath);
        if (!command.ok) {
            uiState_->tapeStatusMessage =
                "HDV mount failed: " + command.error;
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
        }
        if (command.ok) bootHdvImage();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── SmartPort / on-board 3.5" ───────────────────────────────────────────

void MainWindow::renderDisk35PanelWindow()
{
    if (!uiState_->showDisk35Panel) return;
    const auto storageSnapshot =
        storageCoordinator_->captureDisk35(*controller);

    pom2::Disk35Controller_ImGui::PanelSnapshot snap;
    // 3.5" is supported by the //c+ profile or by any profile carrying a
    // SmartPort card. The coordinator exposes the effective target without
    // leaking the selected device kind into this panel.
    snap.supportedByProfile =
        (activeProfile == pom2::SystemProfile::AppleIIcPlus) ||
        storageSnapshot.usesSmartPort();

    // Snapshot and commands deliberately share the coordinator's target
    // selection. This prevents the panel from displaying the on-board pair
    // while mount/eject/write-back operate on SmartPort units or vice versa.
    for (int drive = 0; drive < 2; ++drive) {
        const auto& source = storageSnapshot.drives[drive];
        auto& target = snap.drives[drive];
        target.diskLoaded = source.loaded;
        target.motorOn = source.motorOn;
        target.track = source.track;
        target.side1 = source.side1;
        target.writeProtected = source.writeProtected;
        target.diskPath = source.path;
        target.lastError = source.lastError;
        target.hasUnsavedChanges = source.hasUnsavedChanges;
        target.writeBackEnabled = source.writeBackEnabled;
        target.isWoz = source.isWoz;
        target.convertTargetPath = source.convertTargetPath;
    }

    // Library scan — mirrors the Disk II library scan but only picks
    // up files large enough to be 800K (size sniff via filesystem).
    {
        namespace fs = std::filesystem;
        std::error_code ec;
        for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5",
                                 "disks_5.4",   "../disks_5.4",   "../../disks_5.4" }) {
            if (!fs::is_directory(dir, ec)) continue;
            const fs::path root(dir);
            for (auto it = fs::recursive_directory_iterator(root,
                     fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec))
            {
                const auto& entry = *it;
                const std::string name = entry.path().filename().string();
                if (!name.empty() && name.front() == '.') {
                    if (entry.is_directory(ec)) it.disable_recursion_pending();
                    continue;
                }
                if (!entry.is_regular_file(ec)) continue;
                const std::string ext = entry.path().extension().string();
                if (ext != ".po" && ext != ".2mg" && ext != ".woz") continue;
                const auto sz = entry.file_size(ec);
                if (ec) continue;
                // 800K raw or 2IMG-wrapped (header + 819 200). A `.woz` is
                // FLUX, so its size says nothing about the payload — the
                // 3.5" loader decodes it and refuses a 5.25" one by name.
                if (ext != ".woz" &&
                    sz != 819200 && sz != 819200 + 64 &&
                    !(sz > 819200 && sz < 819200 + 4096)) continue;
                pom2::Disk35Controller_ImGui::LibraryEntry e;
                e.displayName = fs::relative(entry.path(), root, ec).string();
                if (e.displayName.empty()) e.displayName = name;
                e.fullPath = entry.path().string();
                snap.library.push_back(std::move(e));
            }
            if (!snap.library.empty()) break;
        }
        std::sort(snap.library.begin(), snap.library.end(),
                  [](const auto& a, const auto& b) {
                      return a.displayName < b.displayName;
                  });
    }

    ImGui::SetNextWindowPos (ImVec2(1055, 30),  ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(705,  600), ImGuiCond_FirstUseEver);
    // The title identifies the effective target. Its stable per-slot ImGui
    // id also preserves position and size per configuration.
    char disk35Title[64];
    if (storageSnapshot.usesSmartPort()) {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (slot %d)",
                      storageSnapshot.smartPortSlot);
    } else {
        std::snprintf(disk35Title, sizeof(disk35Title),
                      "Disk 3.5\" (//c+ on-board)");
    }
    auto result = disk35Panel->render(
        disk35Title, uiState_->showDisk35Panel, snap);

    if (result.requestConvertDrive >= 0) {
        const auto converted = storageCoordinator_->convertDisk35WozToPo(
            *controller, *settings, result.requestConvertDrive);
        if (converted.ok) {
            pom2::log().info("Disk 3.5",
                "converted WOZ to " + converted.outputPath);
            uiState_->tapeStatusMessage = "Converted to " +
                std::filesystem::path(converted.outputPath).filename().string() +
                " and mounted with write-back on — the .woz is untouched";
        } else {
            uiState_->tapeStatusMessage =
                "Convert failed: " + converted.error;
            pom2::log().warn("Disk 3.5", uiState_->tapeStatusMessage);
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 8.0;
    }

    for (int d = 0; d < 2; ++d) {
        if (result.requestEject[d]) {
            const auto command = storageCoordinator_->ejectDisk35(
                *controller, *settings, d);
            uiState_->tapeStatusMessage = command.ok
                ? (std::string("3.5\" drive ") +
                   (d == 0 ? "1 (internal)" : "2 (external)") + " ejected")
                : ("3.5\" eject failed: " + command.error);
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
        }
        if (result.requestWriteBackToggle[d]) {
            const auto command = storageCoordinator_->setDisk35WriteBack(
                *controller, *settings, d, result.newWriteBack[d]);
            uiState_->tapeStatusMessage = command.ok
                ? std::string("3.5\" drive ") + (d == 0 ? "1" : "2") +
                    (result.newWriteBack[d]
                        ? ": write-back ENABLED (saves on eject)"
                        : ": write-back disabled")
                : "3.5\" write-back failed: " + command.error;
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
        }
    }
    if (result.openMountDialog) {
        disk35Panel->mountDialogOpen     = true;
        disk35Panel->mountDialogForDrive = result.openMountDialogForDrive;
        if (disk35Panel->dialogPath.empty()) disk35Panel->dialogPath = "disks_3.5/";
    }
    if (!result.requestMountPath.empty()) {
        const auto command = storageCoordinator_->mountDisk35(
            *controller, *settings, result.requestMountDrive,
            result.requestMountPath);
        if (command.ok) {
            uiState_->tapeStatusMessage = "3.5\" mounted: " + result.requestMountPath;
        } else {
            uiState_->tapeStatusMessage =
                "3.5\" mount failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
    // Library left-click mounts then boots the authoritative SmartPort slot.
    // The on-board fallback can only restart the machine because POM2 does
    // not model the //c+ IWM boot path yet.
    if (!result.requestInsertAndBoot.empty()) {
        const int d = result.insertAndBootDrive;
        const auto command = storageCoordinator_->mountDisk35(
            *controller, *settings, d, result.requestInsertAndBoot);
        if (command.ok) {
            // SmartPort media boots through its owning slot. The on-board
            // fallback is mounted and restarted, but cannot be called booted
            // until the IWM path is modelled.
            if (command.bootSlot >= 0) {
                controller->bootFromSlot(command.bootSlot);
                uiState_->tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " booted (slot " + std::to_string(command.bootSlot)
                    + "): " + result.requestInsertAndBoot;
            } else {
                controller->coldBoot();
                uiState_->tapeStatusMessage = "3.5\" drive "
                    + std::string(d == 0 ? "1" : "2")
                    + " mounted (did not boot): "
                    + result.requestInsertAndBoot;
            }
        } else {
            uiState_->tapeStatusMessage =
                "3.5\" boot failed: " + command.error;
        }
        uiState_->tapeStatusUntil = uiState_->lastFrameTime + 4.0;
    }
}

void MainWindow::renderDisk35FileDialog()
{
    if (disk35Panel->mountDialogOpen) {
        ImGui::OpenPopup("Mount 3.5\" image");
        disk35Panel->mountDialogOpen = false;
    }
    if (!ImGui::BeginPopupModal("Mount 3.5\" image", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Target drive: %s",
                disk35Panel->mountDialogForDrive == 0
                    ? "1 (internal, //c+ on-board)"
                    : "2 (external, SmartPort daisy-chain)");
    ImGui::TextUnformatted("800K Sony 3.5\" image — .po (raw ProDOS blocks,"
                           " 819 200 B) or .2mg (with 2IMG header).");
    char buf[512] = {0};
    std::snprintf(buf, sizeof(buf), "%s", disk35Panel->dialogPath.c_str());
    if (ImGui::InputText("##Disk35Path", buf, sizeof(buf),
                         ImGuiInputTextFlags_EnterReturnsTrue))
        disk35Panel->dialogPath = buf;
    else
        disk35Panel->dialogPath = buf;

    namespace fs = std::filesystem;
    std::error_code ec;
    for (const char* dir : { "disks_3.5", "../disks_3.5", "../../disks_3.5" }) {
        if (!fs::is_directory(dir, ec)) continue;
        ImGui::Separator();
        ImGui::TextDisabled("%s/", dir);
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file()) continue;
            const std::string ext = entry.path().extension().string();
            if (ext != ".po" && ext != ".2mg" && ext != ".woz") continue;
            const std::string name = entry.path().filename().string();
            if (ImGui::Selectable(name.c_str()))
                disk35Panel->dialogPath = entry.path().string();
        }
        break;
    }

    ImGui::Separator();
    if (ImGui::Button("Mount", ImVec2(120, 0))) {
        if (!disk35Panel->dialogPath.empty()) {
            const auto command = storageCoordinator_->mountDisk35(
                *controller, *settings, disk35Panel->mountDialogForDrive,
                disk35Panel->dialogPath);
            if (command.ok) {
                uiState_->tapeStatusMessage = "3.5\" mounted: " + disk35Panel->dialogPath;
            } else {
                uiState_->tapeStatusMessage =
                    "3.5\" mount failed: " + command.error;
            }
            uiState_->tapeStatusUntil = uiState_->lastFrameTime + 5.0;
        }
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ─── Apple //e keyboard (clickable photo) ────────────────────────────────

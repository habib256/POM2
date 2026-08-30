// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// MainWindow_Media — mount, eject and boot policy, plus the slot-bus queries
// the rest of the frontend asks its questions through.
//
// The split from MainWindow_StoragePanels is deliberate and is the one worth
// keeping: everything here decides WHAT happens to media (which slot an image
// belongs in, whether a card has to be auto-plugged to boot it, what an eject
// commits), and nothing here draws anything. The panels call in; nothing here
// calls back out to ImGui.
//
// Two rules govern the whole file, both learned the hard way and both
// documented in CLAUDE.md:
//
//   * Never hold `stateMutex` across file I/O. Every mount goes through the
//     two-phase form in MediaMount.h — read and decode unlocked, take the
//     lock only to swap the finished object in. A 32 MB image is 12.8 ms of
//     read against a 20 ms PAL frame, and the lock is held by the CPU worker
//     and the UI thread both.
//   * The `primaryX()` / `xCards()` queries walk SlotBus TOPOLOGY only, which
//     is UI-thread-confined, so they do not take the lock. They must stay
//     that way: adding a read of emulated state to one of them would make
//     every caller wrong at once.

#include "MainWindow.h"

#include "Block512Backing.h"
#include "CffaCard.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "Logger.h"
#include "MediaMount.h"
#include "Memory.h"
#include "MountableMediaCard.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "ProDOSVolume.h"
#include "ResourcePaths.h"
#include "Settings.h"
#include "SlotCardCatalog.h"
#include "SlotProvisioningCoordinator.h"
#include "SmartPort35Unit.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "StorageCoordinator.h"
#include "SuperSerialCard.h"
#include "SystemProfile.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

void MainWindow::bootHdvImage()
{
    pom2::ProDOSBlockCard* dev = hdvDevice();
    if (!dev || !dev->isImageLoaded()) {
        tapeStatusMessage = "HDV boot failed: no image loaded";
        tapeStatusUntil   = lastFrameTime + 4.0;
        return;
    }
    // Under the lock, same reason as kioskRescanDisks: this is a reference
    // into live card state that another thread can reassign.
    std::string p;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        p = dev->getImagePath();
    }
    // Boot from the slot the HDV/CFFA card is actually plugged in — the user
    // can move it to slot 2 / 7 / etc. via Slot Configuration and the
    // boot path follows. The card's slot ROM bakes its slot number into
    // the ProDOS dispatcher trampolines, so `bootFromSlot(N)` lands on
    // the right $C(N)00 entry point automatically.
    controller->bootFromSlot(dev->getSlot());
    tapeStatusMessage = "Booting HDV (slot " +
        std::to_string(dev->getSlot()) + "): " + p;
    tapeStatusUntil   = lastFrameTime + 4.0;
}

bool MainWindow::ejectMediaBay(int slot, int index, bool diskII)
{
    // Addressed by SLOT + bay rather than by a card pointer: the status bar
    // builds its rows from a value snapshot taken under the lock and released
    // before drawing, so by the time the user clicks, any pointer captured
    // back then could belong to a card a Slot-Config Apply has since deleted.
    // Re-resolving through the bus here is what makes the click safe.
    std::string label;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lk(controller->stateMutex());
        SlotPeripheral* per = controller->memory().slotBus().peripheral(slot);
        if (!per) return false;

        if (diskII) {
            auto* d2 = dynamic_cast<DiskIICard*>(per);
            if (!d2) return false;
            // ejectDisk() saves dirty tracks first when write-back is on and
            // returns false if that save failed — in which case the medium
            // deliberately stays mounted rather than losing the writes.
            ok = d2->ejectDisk(index);
            label = "slot " + std::to_string(slot) + " drive " +
                    std::to_string(index + 1);
            if (ok) {
                // Match the shutdown-persist keys so the disk does not come
                // back on the next launch (the same pair `restartEmulation
                // FromSettings` snapshots).
                const std::string k = "_slot" + std::to_string(slot);
                settings->setString("disk_path" + k, "");
                if (index == 0 && d2 == primaryDiskII()) settings->setString("disk_path", "");
            }
        } else {
            auto* media = dynamic_cast<pom2::MountableMediaCard*>(per);
            if (!media) return false;
            ok = media->ejectBay(index);
            label = "slot " + std::to_string(slot);
            if (media->bayCount() > 1)
                label += " bay " + std::to_string(index + 1);
            if (ok) {
                // SmartPort units persist their own per-unit key, and it is
                // written eagerly on mount (not at shutdown), so an eject that
                // did not clear it would remount the image on the next launch.
                if (auto* sp = dynamic_cast<pom2::SmartPortCard*>(per)) {
                    settings->setString(
                        "smartport_slot" + std::to_string(sp->getSlot()) +
                        "_unit" + std::to_string(index) + "_path", "");
                }
                if (dynamic_cast<ProDOSHardDiskCard*>(per))
                    settings->setString("hdv_path", "");
                if (auto* cffa = dynamic_cast<pom2::CffaCard*>(per)) {
                    settings->setString(
                        "cffa_slot" + std::to_string(cffa->getSlot()) + "_path",
                        "");
                }
            }
        }
    }
    if (ok) settings->save();

    tapeStatusMessage = ok ? ("Ejected " + label)
                           : ("Eject refused for " + label +
                              " — the image could not be saved, so it stays "
                              "mounted");
    tapeStatusUntil = lastFrameTime + (ok ? 3.0 : 8.0);
    if (!ok) pom2::log().warn("Media", tapeStatusMessage);
    return ok;
}

void MainWindow::ejectAllDisks()
{
    // One locked pass over the whole topology — Disk II (BOTH drives), every
    // block device, every SmartPort unit and the on-board 3.5" pair — with the
    // settings writes applied after the lock is released.
    //
    // Two things the hand-rolled version got wrong. It called `ejectDisk()`
    // with the default argument, so it only ever ejected drive 1 and left
    // drive 2's medium mounted with its path still persisted. And it ignored
    // every eject's return value, so a medium whose write-back failed was
    // reported as ejected: the failure is exactly when the user needs to know,
    // because the card keeps that disk mounted so the write can be retried.
    const auto result = storageCoordinator_->ejectAllMedia(*controller, *settings);

    if (!result.ok()) {
        std::string msg = "Eject failed (media left mounted): ";
        for (size_t i = 0; i < result.failures.size(); ++i)
            msg += (i ? "; " : "") + result.failures[i];
        tapeStatusMessage = std::move(msg);
    } else {
        tapeStatusMessage = result.changed ? "Eject completed"
                                           : "Nothing was mounted";
    }
    tapeStatusUntil = lastFrameTime + 3.0;
}

bool MainWindow::routeMount35(int driveIdx, const std::string& path,
                              std::string& errOut)
{
    // One routing rule for 3.5" media, in the coordinator: a plugged SmartPort
    // card's units own it (auto-creating a SmartPort35Unit on the target bay,
    // flushing whatever was there first so a failed write-back aborts the
    // mount rather than losing the writes), and only without one does it fall
    // through to the //c+ on-board hub. Callers keep this signature; the CLI
    // insert+boot path and the Library share it.
    const auto r = storageCoordinator_->mountDisk35(*controller, *settings,
                                                    driveIdx, path);
    if (!r.ok) errOut = r.error;
    return r.ok;
}

bool MainWindow::routeMountHdv(const std::string& path, int& bootSlotOut,
                               std::string& errOut)
{
    // On //c-class the cffa/hdv slot cards are ROM-masked by the forced
    // INTCXROM and can't boot ($Cn00 reads internal ROM, not the card) —
    // the only bootable block device is the on-board SmartPort (slot 5),
    // so skip the dedicated-HDV-card branch there and route HDV to the
    // SmartPort unit. See project_iic_smartport_boot.
    const bool iicClass =
        (activeProfile == pom2::SystemProfile::AppleIIc ||
         activeProfile == pom2::SystemProfile::AppleIIcPlus ||
         activeProfile == pom2::SystemProfile::AppleIIcPAL);
    // Prefer a dedicated HDV-class card — the MAME-faithful CffaCard if
    // plugged, else the synthetic ProDOSHardDiskCard; else route to a
    // SmartPort card's unit 0 (auto-creating a SmartPortHdvUnit). Promoted
    // from a lambda in renderDiskLibraryWindow.
    if (!iicClass) {
        if (pom2::ProDOSBlockCard* dev = hdvDevice()) {
            // Two-phase: up to 32 MiB read with no lock held, then the lock
            // only for the 2IMG parse and the swap. This was the largest
            // single stall in the tree — 12.8 ms warm-cache, most of a PAL
            // frame with the machine and the window both stopped.
            // Same two-phase read, and the coordinator writes hdv_path with
            // the mount instead of leaving it for the next shutdown.
            const int slot = dev->getSlot();
            const auto r = storageCoordinator_->mountMediaBay(
                *controller, *settings, slot, 0, path);
            if (!r.ok) {
                errOut = r.error;
                hdvStatus = "no image mounted";
                return false;
            }
            hdvPath = path;
            hdvStatus = "loaded: " + path;
            bootSlotOut = slot;
            return true;
        }
    }
    if (primarySmartPortCard()) {
        const std::string base =
            "smartport_slot" + std::to_string(primarySmartPortCard()->getSlot()) +
            "_unit0";
        pom2::SmartPortUnit* u;
        bool replaced = false;
        {
            // Le verrou couvre l'inspection et l'echange d'unite — PAS le
            // montage. mountSmartPortUnit prend stateMutex lui-meme (les deux
            // phases), et stateMutex est non recursif : quand la conversion
            // deux-phases (47f7485) a remplace le loadImage inline, la garde
            // englobante heritee de l'ancien code re-verrouillait le meme
            // mutex dans le meme fil — POM2 gelait sur TOUT boot HDV //c,
            // fenetre comprise. Le pointeur d'unite reste valide hors verrou :
            // la table des unites est confinee au fil UI (voir blockCards()).
            std::lock_guard<std::mutex> lk(controller->stateMutex());
            u = primarySmartPortCard()->unit(0);
            if (!u || u->kindKey() != pom2::SmartPortHdvUnit::kKindKey) {
                // Flush before setUnit destroys it — see routeMount35 above
                // for why the destructor's best-effort save is not enough.
                if (u && !u->saveDirty()) {
                    errOut = "unsaved changes on SmartPort unit 1 could not "
                             "be written: " + u->lastError();
                    return false;
                }
                primarySmartPortCard()->setUnit(
                    0, std::make_unique<pom2::SmartPortHdvUnit>());
                u = primarySmartPortCard()->unit(0);
                replaced = true;
            }
        }
        // Deux phases : lecture sans verrou, echange sous verrou — dans
        // mountSmartPortUnit. Le 3,5" au-dessus reste inline a dessein :
        // son unite n'a pas de dos de blocs, la phase 1 serait perdue.
        if (!pom2::mountSmartPortUnit(*controller, *u, path, errOut))
            return false;
        settings->setString(base + "_type",
            std::string(pom2::SmartPortHdvUnit::kKindKey));
        settings->setString(base + "_path", path);
        if (replaced) settings->setBool(base + "_writeback", false);
        if (!settingsReadOnly()) settings->save();   // kiosk: never touch state.cfg
        bootSlotOut = primarySmartPortCard()->getSlot();
        return true;
    }
    errOut = "no HDV or SmartPort card plugged";
    return false;
}

pom2::ProDOSBlockCard* MainWindow::hdvDevice() const
{
    // The CFFA-outranks-HDV preference is the coordinator's rule now, so the
    // menus, the AI server and the boot path cannot drift apart on it.
    return storageCoordinator_->topology(controller->memory().slotBus())
        .preferredBlock();
}

// Bus *topology* reads (which slot holds which card) are UI-thread-confined:
// every writer — plugSlotsFromSettings, applyProfile, the slot-config
// rebuild — runs on this thread, and the worker only ever reads the table.
// A lock here would protect nothing while reading as though it did; per-card
// *state* is a different matter and does go through lockState().
std::vector<pom2::ProDOSBlockCard*> MainWindow::blockCards() const
{
    // Walk the bus (slots 1..7) and cross-cast each plugged peripheral to
    // the ProDOSBlockCard mix-in. Both implementers (ProDOSHardDiskCard,
    // CffaCard) inherit SlotPeripheral *and* ProDOSBlockCard, so the
    // dynamic_cast side-cast succeeds for exactly those and yields nullptr
    // for everything else. Slot order is ascending, matching the "lowest
    // slot is primary" convention used for primaryDiskII()/primaryHdvCard().
    return storageCoordinator_->topology(controller->memory().slotBus())
        .blockCards;
}

std::vector<pom2::SmartPortCard*> MainWindow::smartPortCards() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .smartPortCards;
}

std::vector<DiskIICard*> MainWindow::diskIICards() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .diskIICards;
}

std::vector<SuperSerialCard*> MainWindow::serialCards() const
{
    // Slot-ascending, which is what makes "the primary is the first" true.
    std::vector<SuperSerialCard*> out;
    SlotBus& bus = controller->memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot)
        if (auto* ssc = dynamic_cast<SuperSerialCard*>(bus.peripheral(slot)))
            out.push_back(ssc);
    return out;
}

SuperSerialCard* MainWindow::primarySerialCard() const
{
    const auto cards = serialCards();
    return cards.empty() ? nullptr : cards.front();
}

DiskIICard* MainWindow::primaryDiskII() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryDiskII;
}

ProDOSHardDiskCard* MainWindow::primaryHdvCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryHdv;
}

pom2::CffaCard* MainWindow::primaryCffaCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primaryCffa;
}

pom2::SmartPortCard* MainWindow::primarySmartPortCard() const
{
    return storageCoordinator_->topology(controller->memory().slotBus())
        .primarySmartPort;
}

bool MainWindow::flushSlotMedia(std::string& err)
{
    // The coordinator walks the same three families (Disk II pending writes,
    // block devices, SmartPort units) and aggregates the same failure text.
    // It is the flush half of the rebuild transaction — only a successful one
    // may prepare a teardown — so it has to be the same code as the one the
    // rebuild path uses, not a second copy that can drift.
    std::lock_guard<std::mutex> lk(controller->stateMutex());
    return storageCoordinator_->flushAll(controller->memory().slotBus(), err);
}

int MainWindow::ensureHdvCardForBoot()
{
    // Preference order and the session-only marking both live in the
    // coordinator, so the CLI, drag-and-drop and Floppy Emu paths cannot
    // disagree about which target a boot should use.
    //
    // Behaviour change, deliberate: on a //c-class profile this now REFUSES
    // when no SmartPort target exists, instead of falling through to plug an
    // HDV card. Slot cards there are ROM-masked by the forced INTCXROM and
    // cannot boot ($Cn00 reads internal ROM, not the card), so the old
    // fallback conjured a card that could never be booted from and then
    // reported success.
    const auto r = slotProvisioningCoordinator_->ensureHdvBootTarget(
        *controller, *settings, activeProfile);
    if (!r && !r.error.empty()) pom2::log().warn("Slots", r.error);
    return r.slot;
}

int MainWindow::ensureSmartPortCardForBoot()
{
    const auto r = slotProvisioningCoordinator_->ensureSmartPortBootTarget(
        *controller, activeProfile);
    if (!r && !r.error.empty()) pom2::log().warn("Slots", r.error);
    return r.slot;
}

bool MainWindow::insertAndBootImage(const std::string& path, std::string& errOut)
{
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
            for (auto* c : diskIICards()) if (c && c->getSlot() == 6) { target = c; break; }
            if (!target) target = primaryDiskII();
            if (!target) { errOut = "no Disk II card in the current config"; return false; }
            const bool ok = pom2::mountDiskII(*controller, *target, 0, path,
                                              errOut, /*seekTrack0=*/true);
            if (!ok) return false;
            if (!controller->bootFromSlot(target->getSlot())) {
                errOut = "slot " + std::to_string(target->getSlot()) +
                         " did not boot the image (cold-booted instead)";
                return false;
            }
            controller->setMode(EmulationController::Mode::Running);
            return true;
        }
        case DiskSlotClass::Sony35: {
            // Without a SmartPort card, routeMount35 falls through to the
            // //c+ on-board Sony hub — a device that only exists on the
            // //c+. On any other machine the image would "mount" into
            // hardware the guest can't see and the cold boot below would
            // land at the BASIC prompt with no error at all.
            //
            // When neither exists, auto-plug a Liron-class SmartPort the
            // same way the HDV branch below auto-plugs a block card: a
            // dropped 800K .po/.2mg is explicit "boot this" intent, and
            // failing it on the stock II+/IIe config (which ships no
            // SmartPort) made drag-and-drop refuse the single most common
            // 3.5" distribution format. Session-local, never persisted.
            if (!primarySmartPortCard() &&
                activeProfile != pom2::SystemProfile::AppleIIcPlus &&
                ensureSmartPortCardForBoot() < 0) {
                errOut = "no 3.5\" device in this config, and no free slot "
                         "to plug a SmartPort 3.5\" card into";
                return false;
            }
            if (!routeMount35(0, path, errOut)) return false;
            // SmartPort card present (incl. //c-class built-in slot 5) →
            // boot it explicitly; otherwise cold-boot (//c+ on-board hub).
            if (primarySmartPortCard()) {
                if (!controller->bootFromSlot(primarySmartPortCard()->getSlot())) {
                    errOut = "slot " + std::to_string(primarySmartPortCard()->getSlot()) +
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
            if (ensureHdvCardForBoot() < 0) {
                errOut = "no free slot to plug an HDV card into";
                return false;
            }
            int bootSlot = 0;
            if (!routeMountHdv(path, bootSlot, errOut)) return false;
            if (!controller->bootFromSlot(bootSlot)) {
                errOut = "slot " + std::to_string(bootSlot) +
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

bool MainWindow::mountProDOSFolder(const std::string& path, std::string& errOut)
{
    if (ensureHdvCardForBoot() < 0 || !primaryHdvCard()) {
        errOut = "no free slot to plug an HDV card into";
        return false;
    }

    std::vector<std::uint8_t> bytes;
    const auto built = pom2::buildVolumeFromFolder(path, "HOST", bytes);
    if (!built.ok) {
        errOut = built.error;
        return false;
    }

    const auto mounted = storageCoordinator_->mountBlockBytes(
        *controller, *settings, primaryHdvCard()->getSlot(), std::move(bytes),
        std::string("[host folder] ") + path, path);
    if (!mounted.ok) {
        errOut = mounted.error;
        return false;
    }

    pom2::log().info("CLI", "mounted /HOST/ from " + path + " (" +
        std::to_string(built.filesIncluded) + " files, " +
        std::to_string(built.totalBlocks) + " blocks)");
    return true;
}

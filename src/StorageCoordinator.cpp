// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "StorageCoordinator.h"

#include "CffaCard.h"
#include "Disk35Image.h"
#include "DiskIICard.h"
#include "DiskImage.h"
#include "EmulationController.h"
#include "MountableMediaCard.h"
#include "ProDOSBlockCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SmartPortCard.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"
#include "SmartPortUnit.h"
#include "Sony35Drive.h"

#include <algorithm>
#include <filesystem>
#include <memory>
#include <utility>

namespace pom2 {
namespace {

SmartPortCard* smartPortAt(SlotBus& bus, int requestedSlot = -1)
{
    if (requestedSlot >= 1 && requestedSlot < SlotBus::kSlotCount)
        return dynamic_cast<SmartPortCard*>(bus.peripheral(requestedSlot));
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        if (auto* card = dynamic_cast<SmartPortCard*>(bus.peripheral(slot)))
            return card;
    }
    return nullptr;
}

struct SettingUpdate {
    std::string key;
    std::string stringValue;
    bool boolValue = false;
    bool isBool = false;
};

std::string diskIIPathSettingKey(int slot, std::size_t drive)
{
    std::string key = "disk_path_slot" + std::to_string(slot);
    if (drive > 0) key += "_drive" + std::to_string(drive + 1);
    return key;
}

void appendStringSetting(std::vector<SettingUpdate>& updates,
                         std::string key, std::string value)
{
    updates.push_back({std::move(key), std::move(value), false, false});
}

void appendBoolSetting(std::vector<SettingUpdate>& updates,
                       std::string key, bool value)
{
    updates.push_back({std::move(key), {}, value, true});
}

void applySettingUpdates(Settings& settings,
                         const std::vector<SettingUpdate>& updates)
{
    for (const auto& update : updates) {
        if (update.isBool)
            settings.setBool(update.key, update.boolValue);
        else
            settings.setString(update.key, update.stringValue);
    }
}

void appendDiskIIDriveSettingUpdates(
    std::vector<SettingUpdate>& updates, const SlotBus& bus,
    const DiskIICard& card, int drive)
{
    const std::string path = card.isDiskLoaded(drive)
        ? std::string(card.getDiskPath(drive)) : std::string();
    appendStringSetting(
        updates, diskIIPathSettingKey(card.getSlot(), drive), path);
    appendBoolSetting(
        updates, "disk_writeback_slot" + std::to_string(card.getSlot()),
        card.isWriteBackEnabled());

    const auto cards = StorageCoordinator{}.topology(bus);
    if (drive == 0 && cards.primaryDiskII == &card) {
        appendStringSetting(updates, "disk_path", path);
        appendBoolSetting(updates, "disk_writeback",
                          card.isWriteBackEnabled());
    }
}

bool appendMediaBaySettingUpdates(
    std::vector<SettingUpdate>& updates, SlotPeripheral& peripheral,
    int slot, int bay, int autoHdvSlot, int autoSmartPortSlot)
{
    if (auto* card = dynamic_cast<SmartPortCard*>(&peripheral)) {
        if (slot == autoSmartPortSlot) return false;
        if (bay < 0 || bay >= static_cast<int>(SmartPortCard::kMaxUnits))
            return false;
        const std::string base = "smartport_slot" + std::to_string(slot) +
                                 "_unit" + std::to_string(bay);
        const SmartPortUnit* unit = card->unit(static_cast<std::size_t>(bay));
        appendStringSetting(
            updates, base + "_type",
            unit ? std::string(unit->kindKey()) : std::string());
        appendStringSetting(
            updates, base + "_path", unit ? unit->path() : std::string());
        appendBoolSetting(
            updates, base + "_writeback",
            unit ? unit->isWriteBackEnabled() : false);
        return true;
    }
    if (auto* card = dynamic_cast<CffaCard*>(&peripheral)) {
        const std::string base = "cffa_slot" + std::to_string(slot);
        appendStringSetting(updates, base + "_path", card->getImagePath());
        appendBoolSetting(
            updates, base + "_writeback", card->isWriteBackEnabled());
        return true;
    }
    if (auto* card = dynamic_cast<ProDOSHardDiskCard*>(&peripheral)) {
        // Session-only auto-provisioned cards and synthetic host-folder
        // volumes must not overwrite the user's configured HDV.
        if (slot == autoHdvSlot ||
            card->getImagePath().rfind("[host folder] ", 0) == 0) {
            return false;
        }
        appendStringSetting(updates, "hdv_path", card->getImagePath());
        appendBoolSetting(
            updates, "hdv_writeback", card->isWriteBackEnabled());
        return true;
    }
    return false;
}

bool mountSmartPortUnitAs(
    SmartPortCard& card, int bay, std::string_view kind,
    const std::string& path, std::vector<SettingUpdate>& updates,
    std::string& error, int autoSmartPortSlot)
{
    if (bay < 0 || bay >= static_cast<int>(SmartPortCard::kMaxUnits)) {
        error = "invalid SmartPort unit " + std::to_string(bay + 1);
        return false;
    }
    const auto index = static_cast<std::size_t>(bay);
    SmartPortUnit* unit = card.unit(index);
    if (!unit || unit->kindKey() != kind) {
        // Replacing the type destroys the current unit. Flush explicitly so
        // a failed write-back leaves the only dirty copy mounted for retry.
        if (unit && !unit->saveDirty()) {
            error = "unsaved changes on SmartPort unit " +
                    std::to_string(bay + 1) +
                    " could not be written: " + unit->lastError();
            return false;
        }
        auto replacement = makeSmartPortUnit(kind);
        if (!replacement) {
            error = "unsupported SmartPort media type '" +
                    std::string(kind) + "'";
            return false;
        }
        card.setUnit(index, std::move(replacement));
        unit = card.unit(index);
    }
    if (!unit || !unit->loadImage(path)) {
        error = unit ? unit->lastError() : "SmartPort unit creation failed";
        return false;
    }
    (void)appendMediaBaySettingUpdates(
        updates, card, card.getSlot(), bay, -1, autoSmartPortSlot);
    return true;
}

std::string freePoNameFor(const std::string& wozPath)
{
    if (wozPath.empty()) return {};
    const std::filesystem::path source(wozPath);
    const auto directory = source.parent_path();
    const std::string stem = source.stem().string();
    std::error_code error;
    for (int number = 1; number <= 99; ++number) {
        const std::string name = number == 1
            ? stem + ".po"
            : stem + " (" + std::to_string(number) + ").po";
        const auto candidate = directory / name;
        if (!std::filesystem::exists(candidate, error))
            return candidate.string();
        error.clear();
    }
    return {};
}

void copyDisk35ImageState(
    StorageCoordinator::Disk35DriveSnapshot& target,
    const Disk35Image& image)
{
    target.loaded = image.isLoaded();
    target.writeProtected = image.isWriteProtected();
    target.path = image.path();
    target.lastError = image.lastError();
    target.hasUnsavedChanges = image.hasUnsavedChanges();
    target.writeBackEnabled = image.isWriteBackEnabled();
    target.isWoz = image.kind() == Disk35Image::ImageKind::Woz35;
}

StorageCoordinator::MediaCommandResult commandError(std::string error)
{
    StorageCoordinator::MediaCommandResult result;
    result.error = std::move(error);
    return result;
}

} // namespace

DiskIICard* StorageCoordinator::Topology::diskIIAt(int slot) const noexcept
{
    for (auto* card : diskIICards) {
        if (card && card->getSlot() == slot) return card;
    }
    return nullptr;
}

ProDOSBlockCard*
StorageCoordinator::Topology::preferredBlock() const noexcept
{
    if (primaryCffa) return static_cast<ProDOSBlockCard*>(primaryCffa);
    return static_cast<ProDOSBlockCard*>(primaryHdv);
}

StorageCoordinator::Topology
StorageCoordinator::topology(const SlotBus& bus) const
{
    Topology result;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (auto* card = dynamic_cast<DiskIICard*>(peripheral)) {
            result.diskIICards.push_back(card);
            if (!result.primaryDiskII) result.primaryDiskII = card;
        }
        if (auto* card = dynamic_cast<ProDOSBlockCard*>(peripheral))
            result.blockCards.push_back(card);
        if (auto* card = dynamic_cast<ProDOSHardDiskCard*>(peripheral)) {
            if (!result.primaryHdv) result.primaryHdv = card;
        }
        if (auto* card = dynamic_cast<CffaCard*>(peripheral)) {
            if (!result.primaryCffa) result.primaryCffa = card;
        }
        if (auto* card = dynamic_cast<SmartPortCard*>(peripheral)) {
            result.smartPortCards.push_back(card);
            if (!result.primarySmartPort) result.primarySmartPort = card;
        }
    }
    return result;
}

StorageCoordinator::InventorySnapshot
StorageCoordinator::captureInventory(EmulationController& controller) const
{
    InventorySnapshot snapshot;
    auto state = controller.lockState();
    const auto cards = topology(state.memory().slotBus());
    for (auto* card : cards.diskIICards)
        snapshot.diskIISlots.push_back(card->getSlot());
    for (auto* card : cards.blockCards)
        snapshot.blockSlots.push_back(card->getSlot());
    for (auto* card : cards.smartPortCards)
        snapshot.smartPortSlots.push_back(card->getSlot());
    if (cards.primaryDiskII) {
        snapshot.primaryDiskIISlot = cards.primaryDiskII->getSlot();
        snapshot.primaryDiskUsingBitLss = cards.primaryDiskII->usingBitLss();
        snapshot.primaryDiskLoaded = cards.primaryDiskII->isDiskLoaded();
    }
    if (cards.primaryHdv) {
        snapshot.primaryHdvSlot = cards.primaryHdv->getSlot();
        snapshot.primaryHdvLoaded = cards.primaryHdv->isImageLoaded();
    }
    if (cards.primaryCffa)
        snapshot.primaryCffaSlot = cards.primaryCffa->getSlot();
    if (cards.primarySmartPort) {
        snapshot.primarySmartPortSlot = cards.primarySmartPort->getSlot();
        snapshot.primarySmartPortLironRomLoaded =
            cards.primarySmartPort->isLironRomLoaded();
    }
    return snapshot;
}

StorageCoordinator::RebuildSnapshot
StorageCoordinator::captureRebuildSnapshot(const SlotBus& bus) const
{
    static_assert(DiskIICard::kDriveCount ==
                  static_cast<int>(kDiskIIDriveCount));
    RebuildSnapshot snapshot;
    const auto cards = topology(bus);

    snapshot.diskII.reserve(cards.diskIICards.size());
    for (const auto* card : cards.diskIICards) {
        if (!card) continue;
        DiskIISnapshot disk;
        disk.slot = card->getSlot();
        disk.writeBackEnabled = card->isWriteBackEnabled();
        for (std::size_t drive = 0; drive < disk.drives.size(); ++drive) {
            auto& medium = disk.drives[drive];
            medium.loaded = card->isDiskLoaded(static_cast<int>(drive));
            if (medium.loaded)
                medium.path = card->getDiskPath(static_cast<int>(drive));
        }
        snapshot.diskII.push_back(std::move(disk));
    }

    if (cards.primaryHdv) {
        SlotMediumSnapshot medium;
        medium.slot = cards.primaryHdv->getSlot();
        medium.loaded = cards.primaryHdv->isImageLoaded();
        if (medium.loaded) medium.path = cards.primaryHdv->getImagePath();
        medium.writeBackEnabled = cards.primaryHdv->isWriteBackEnabled();
        snapshot.primaryHdv = std::move(medium);
    }

    snapshot.cffa.reserve(cards.blockCards.size());
    for (const auto* block : cards.blockCards) {
        const auto* card = dynamic_cast<const CffaCard*>(block);
        if (!card) continue;
        SlotMediumSnapshot medium;
        medium.slot = card->getSlot();
        medium.loaded = card->isImageLoaded();
        if (medium.loaded) medium.path = card->getImagePath();
        medium.writeBackEnabled = card->isWriteBackEnabled();
        snapshot.cffa.push_back(std::move(medium));
    }

    return snapshot;
}

void StorageCoordinator::persistRebuildSettings(
    Settings& settings, const RebuildSnapshot& snapshot) const
{
    for (const auto& disk : snapshot.diskII) {
        for (std::size_t drive = 0; drive < disk.drives.size(); ++drive) {
            const auto& medium = disk.drives[drive];
            settings.setString(diskIIPathSettingKey(disk.slot, drive),
                               medium.loaded ? medium.path : std::string());
        }
        settings.setBool("disk_writeback_slot" + std::to_string(disk.slot),
                         disk.writeBackEnabled);
    }

    if (snapshot.primaryHdv &&
        snapshot.primaryHdv->slot != autoHdvSlot_ &&
        snapshot.primaryHdv->loaded &&
        snapshot.primaryHdv->path.rfind("[host folder] ", 0) ==
            std::string::npos) {
        settings.setString("hdv_path", snapshot.primaryHdv->path);
    }

    for (const auto& medium : snapshot.cffa) {
        const std::string key =
            "cffa_slot" + std::to_string(medium.slot);
        settings.setString(key + "_path",
                           medium.loaded ? medium.path : std::string());
        settings.setBool(key + "_writeback", medium.writeBackEnabled);
    }
}

void StorageCoordinator::persistSessionSettings(
    Settings& settings, const RebuildSnapshot& snapshot) const
{
    persistRebuildSettings(settings, snapshot);

    // The lowest-slot Disk II is the legacy primary. Keep the unsuffixed
    // aliases for older settings consumers; drive 2 never had such an alias.
    const DiskIISnapshot* primaryDisk = nullptr;
    for (const auto& disk : snapshot.diskII) {
        if (!primaryDisk || disk.slot < primaryDisk->slot)
            primaryDisk = &disk;
    }
    if (primaryDisk) {
        const auto& drive1 = primaryDisk->drives[0];
        settings.setString("disk_path",
                           drive1.loaded ? drive1.path : std::string());
        settings.setBool("disk_writeback", primaryDisk->writeBackEnabled);
    }

    if (snapshot.primaryHdv && snapshot.primaryHdv->slot != autoHdvSlot_) {
        const bool persistable = snapshot.primaryHdv->loaded &&
            snapshot.primaryHdv->path.rfind("[host folder] ", 0) ==
                std::string::npos;
        settings.setString("hdv_path",
                           persistable ? snapshot.primaryHdv->path
                                       : std::string());
        settings.setBool("hdv_writeback",
                         snapshot.primaryHdv->writeBackEnabled);
    } else {
        settings.setString("hdv_path", "");
    }
}

bool StorageCoordinator::persistDiskIIDrive(
    Settings& settings, const SlotBus& bus, int slot, int drive) const
{
    if (slot < 1 || slot >= SlotBus::kSlotCount || drive < 0 ||
        drive >= DiskIICard::kDriveCount) {
        return false;
    }
    auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
    if (!card) return false;

    const std::string path = card->isDiskLoaded(drive)
        ? std::string(card->getDiskPath(drive)) : std::string();
    settings.setString(
        diskIIPathSettingKey(slot, static_cast<std::size_t>(drive)), path);
    settings.setBool("disk_writeback_slot" + std::to_string(slot),
                     card->isWriteBackEnabled());

    if (drive == 0 && topology(bus).primaryDiskII == card) {
        settings.setString("disk_path", path);
        settings.setBool("disk_writeback", card->isWriteBackEnabled());
    }
    return true;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::mountDiskII(
    EmulationController& controller, Settings& settings, int slot, int drive,
    const std::string& path, bool seekTrackZero) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    if (!DiskIICard::validDrive(drive))
        return commandError("invalid Disk II drive " +
                            std::to_string(drive + 1));

    // Phase 1, NO lock: read the file and run the nibble decode. Doing this
    // under stateMutex is what MediaMount.h exists to prevent — the worker
    // takes that lock every 4096 cycles and the UI takes it to paint, so a
    // 12.8 ms warm-cache read freezes the machine and the window together.
    //
    // The write-back flag is read unlocked on purpose: prepareDisk only needs
    // it to decide whether the decode keeps its write buffers, and phase 2
    // re-checks the card it actually installs into.
    bool writeBack = false;
    {
        auto state = controller.lockState();
        auto* card = dynamic_cast<DiskIICard*>(
            state.memory().slotBus().peripheral(slot));
        if (!card)
            return commandError("no Disk II card in slot " +
                                std::to_string(slot));
        writeBack = card->isWriteBackEnabled();
    }
    auto prepared = std::make_unique<DiskImage>();
    if (!DiskIICard::prepareDisk(path, writeBack, *prepared, result.error))
        return result;

    // Phase 2 — re-resolve by SLOT, never through a pointer carried across
    // the gap. This is the caller contract MediaMount.h spells out for
    // anything that is not plain UI-thread code: the bus can have been
    // rebuilt while the read was running.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
        if (!card)
            return commandError("Disk II card in slot " +
                                std::to_string(slot) +
                                " went away during the mount");
        if (!card->installDisk(drive, std::move(*prepared)))
            return commandError(card->getLastError(drive));
        if (seekTrackZero) card->seekTrack0();
        appendDiskIIDriveSettingUpdates(updates, bus, *card, drive);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::ejectDiskII(
    EmulationController& controller, Settings& settings, int slot,
    int drive) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
        if (!card)
            return commandError("no Disk II card in slot " +
                                std::to_string(slot));
        if (!DiskIICard::validDrive(drive))
            return commandError("invalid Disk II drive " +
                                std::to_string(drive + 1));
        if (!card->ejectDisk(drive))
            return commandError(card->getLastError(drive));
        appendDiskIIDriveSettingUpdates(updates, bus, *card, drive);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult
StorageCoordinator::setDiskIIWriteBack(
    EmulationController& controller, Settings& settings, int slot,
    bool enabled) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(slot));
        if (!card)
            return commandError("no Disk II card in slot " +
                                std::to_string(slot));
        card->setWriteBackEnabled(enabled);
        appendDiskIIDriveSettingUpdates(updates, bus, *card, 0);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::mountMediaBay(
    EmulationController& controller, Settings& settings, int slot, int bay,
    const std::string& path) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        if (!media->mountBay(bay, path, result.error)) return result;
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::mountBlockBytes(
    EmulationController& controller, Settings& settings, int slot,
    std::vector<std::uint8_t> bytes, const std::string& label,
    const std::string& hostFolder) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* card = dynamic_cast<ProDOSBlockCard*>(peripheral);
        if (!card)
            return commandError("slot " + std::to_string(slot) +
                                " has no ProDOS block device");
        if (!card->loadImageFromBytes(std::move(bytes), label, hostFolder))
            return commandError(card->getLastError());
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, 0,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::ejectMediaBay(
    EmulationController& controller, Settings& settings, int slot,
    int bay) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        if (!media->ejectBay(bay)) {
            const auto info = media->bayInfo(bay);
            return commandError(info.lastError.empty()
                ? "the image could not be saved" : info.lastError);
        }
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult
StorageCoordinator::setMediaBayWriteBack(
    EmulationController& controller, Settings& settings, int slot, int bay,
    bool enabled) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        media->setBayWriteBack(bay, enabled);
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::setMediaBayType(
    EmulationController& controller, Settings& settings, int slot, int bay,
    const std::string& kind) const
{
    MediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* peripheral = bus.peripheral(slot);
        auto* media = dynamic_cast<MountableMediaCard*>(peripheral);
        if (!media)
            return commandError("slot " + std::to_string(slot) +
                                " has no mountable media");
        if (bay < 0 || bay >= media->bayCount())
            return commandError("invalid media bay " +
                                std::to_string(bay + 1));
        const auto options = media->bayTypeOptions(bay);
        const bool supported = std::any_of(
            options.begin(), options.end(), [&](const auto& option) {
                return option.first == kind;
            });
        if (!supported)
            return commandError("unsupported media type '" + kind + "'");
        media->setBayType(bay, kind);
        (void)appendMediaBaySettingUpdates(
            updates, *peripheral, slot, bay,
            autoHdvSlot_, autoSmartPortSlot_);
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::Disk35Snapshot StorageCoordinator::captureDisk35(
    EmulationController& controller) const
{
    Disk35Snapshot snapshot;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            snapshot.smartPortSlot = cards.primarySmartPort->getSlot();
            for (int drive = 0; drive < 2; ++drive) {
                const auto* unit = dynamic_cast<const SmartPort35Unit*>(
                    cards.primarySmartPort->unit(
                        static_cast<std::size_t>(drive)));
                if (!unit) continue;
                copyDisk35ImageState(snapshot.drives[drive], unit->image());
            }
        } else {
            const Sony35Drive* drives[2] = {
                &controller.sony35Internal(), &controller.sony35External(),
            };
            const Disk35Image* images[2] = {
                &controller.disk35Internal(), &controller.disk35External(),
            };
            for (int drive = 0; drive < 2; ++drive) {
                auto& target = snapshot.drives[drive];
                copyDisk35ImageState(target, *images[drive]);
                target.loaded = drives[drive]->isInserted();
                target.motorOn = drives[drive]->isMotorOn();
                target.track = drives[drive]->track();
                target.side1 = drives[drive]->side1();
                target.writeProtected = drives[drive]->isWriteProtected();
            }
        }
    }
    for (auto& drive : snapshot.drives) {
        if (drive.isWoz) drive.convertTargetPath = freePoNameFor(drive.path);
    }
    return snapshot;
}

StorageCoordinator::RoutedMediaCommandResult
StorageCoordinator::mountDisk35(
    EmulationController& controller, Settings& settings, int drive,
    const std::string& path) const
{
    RoutedMediaCommandResult result;
    if (drive < 0 || drive >= 2) {
        result.error = "invalid 3.5-inch drive " +
                       std::to_string(drive + 1);
        return result;
    }

    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            result.usesSmartPort = true;
            result.bootSlot = cards.primarySmartPort->getSlot();
            result.ok = mountSmartPortUnitAs(
                *cards.primarySmartPort, drive, SmartPort35Unit::kKindKey,
                path, updates, result.error, autoSmartPortSlot_);
        }
    }
    if (result.usesSmartPort) {
        applySettingUpdates(settings, updates);
        if (!updates.empty()) (void)settings.save();
        return result;
    }

    result.ok = controller.mount35(drive, path);
    if (!result.ok) {
        auto state = controller.lockState();
        const auto& image = drive == 0
            ? controller.disk35Internal() : controller.disk35External();
        result.error = image.lastError();
        if (result.error.empty()) result.error = "3.5-inch mount failed";
    }
    return result;
}

StorageCoordinator::MediaCommandResult StorageCoordinator::ejectDisk35(
    EmulationController& controller, Settings& settings, int drive) const
{
    MediaCommandResult result;
    if (drive < 0 || drive >= 2)
        return commandError("invalid 3.5-inch drive " +
                            std::to_string(drive + 1));

    bool usesSmartPort = false;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            usesSmartPort = true;
            auto* unit = dynamic_cast<SmartPort35Unit*>(
                cards.primarySmartPort->unit(
                    static_cast<std::size_t>(drive)));
            if (!unit)
                return commandError("SmartPort unit " +
                    std::to_string(drive + 1) +
                    " is not a 3.5-inch drive");
            if (!unit->eject()) return commandError(unit->lastError());
            (void)appendMediaBaySettingUpdates(
                updates, *cards.primarySmartPort,
                cards.primarySmartPort->getSlot(), drive,
                autoHdvSlot_, autoSmartPortSlot_);
            result.ok = true;
        }
    }
    if (usesSmartPort) {
        applySettingUpdates(settings, updates);
        if (!updates.empty()) (void)settings.save();
        return result;
    }

    result.ok = controller.eject35(drive);
    if (!result.ok) {
        auto state = controller.lockState();
        const auto& image = drive == 0
            ? controller.disk35Internal() : controller.disk35External();
        result.error = image.lastError();
        if (result.error.empty()) result.error = "3.5-inch eject failed";
    }
    return result;
}

StorageCoordinator::MediaCommandResult
StorageCoordinator::setDisk35WriteBack(
    EmulationController& controller, Settings& settings, int drive,
    bool enabled) const
{
    MediaCommandResult result;
    if (drive < 0 || drive >= 2)
        return commandError("invalid 3.5-inch drive " +
                            std::to_string(drive + 1));

    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            auto* unit = dynamic_cast<SmartPort35Unit*>(
                cards.primarySmartPort->unit(
                    static_cast<std::size_t>(drive)));
            if (!unit)
                return commandError("SmartPort unit " +
                    std::to_string(drive + 1) +
                    " is not a 3.5-inch drive");
            unit->setWriteBackEnabled(enabled);
            (void)appendMediaBaySettingUpdates(
                updates, *cards.primarySmartPort,
                cards.primarySmartPort->getSlot(), drive,
                autoHdvSlot_, autoSmartPortSlot_);
        } else {
            auto& image = drive == 0
                ? controller.disk35Internal() : controller.disk35External();
            image.setWriteBackEnabled(enabled);
        }
        result.ok = true;
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::RoutedMediaCommandResult
StorageCoordinator::convertDisk35WozToPo(
    EmulationController& controller, Settings& settings, int drive) const
{
    RoutedMediaCommandResult result;
    if (drive < 0 || drive >= 2) {
        result.error = "invalid 3.5-inch drive " +
                       std::to_string(drive + 1);
        return result;
    }

    Disk35Image imageSnapshot;
    {
        auto state = controller.lockState();
        const auto cards = topology(state.memory().slotBus());
        if (cards.primarySmartPort) {
            const auto* unit = dynamic_cast<const SmartPort35Unit*>(
                cards.primarySmartPort->unit(
                    static_cast<std::size_t>(drive)));
            if (!unit) {
                result.error = "SmartPort unit " +
                    std::to_string(drive + 1) +
                    " is not a 3.5-inch drive";
                return result;
            }
            imageSnapshot = unit->image();
            result.usesSmartPort = true;
            result.bootSlot = cards.primarySmartPort->getSlot();
        } else {
            imageSnapshot = drive == 0
                ? controller.disk35Internal() : controller.disk35External();
        }
    }
    if (imageSnapshot.kind() != Disk35Image::ImageKind::Woz35) {
        result.error = "that drive does not hold a 3.5-inch WOZ";
        return result;
    }
    result.outputPath = freePoNameFor(imageSnapshot.path());
    if (result.outputPath.empty()) {
        result.error = "no free .po filename beside the WOZ";
        return result;
    }
    if (!imageSnapshot.exportRawTo(result.outputPath, result.error))
        return result;

    const auto mounted = mountDisk35(
        controller, settings, drive, result.outputPath);
    if (!mounted.ok) {
        result.error = "converted, but mounting failed: " + mounted.error;
        return result;
    }
    const auto writeBack = setDisk35WriteBack(
        controller, settings, drive, true);
    if (!writeBack.ok) {
        result.error = "converted and mounted, but write-back failed: " +
                       writeBack.error;
        return result;
    }
    result.ok = true;
    result.bootSlot = mounted.bootSlot;
    result.usesSmartPort = mounted.usesSmartPort;
    return result;
}

StorageCoordinator::RoutedMediaCommandResult StorageCoordinator::mountHdv(
    EmulationController& controller, Settings& settings,
    const std::string& path, bool smartPortOnly) const
{
    RoutedMediaCommandResult result;
    std::vector<SettingUpdate> updates;
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        const auto cards = topology(bus);
        if (!smartPortOnly) {
            if (auto* card = cards.preferredBlock()) {
                auto* peripheral = bus.peripheral(card->getSlot());
                if (!card->mountBay(0, path, result.error)) return result;
                if (peripheral) {
                    (void)appendMediaBaySettingUpdates(
                        updates, *peripheral, card->getSlot(), 0,
                        autoHdvSlot_, autoSmartPortSlot_);
                }
                result.bootSlot = card->getSlot();
                result.ok = true;
            }
        }
        if (!result.ok && cards.primarySmartPort) {
            result.usesSmartPort = true;
            result.bootSlot = cards.primarySmartPort->getSlot();
            result.ok = mountSmartPortUnitAs(
                *cards.primarySmartPort, 0, SmartPortHdvUnit::kKindKey,
                path, updates, result.error, autoSmartPortSlot_);
        }
        if (!result.ok && result.error.empty())
            result.error = "no HDV or SmartPort card plugged";
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();
    return result;
}

StorageCoordinator::EjectAllResult StorageCoordinator::ejectAllMedia(
    EmulationController& controller, Settings& settings) const
{
    EjectAllResult result;
    std::vector<SettingUpdate> updates;
    std::array<bool, 2> onboardDisk35Loaded{};
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        const auto cards = topology(bus);

        for (auto* card : cards.diskIICards) {
            if (!card) continue;
            for (int drive = 0; drive < DiskIICard::kDriveCount; ++drive) {
                if (!card->isDiskLoaded(drive)) continue;
                if (card->ejectDisk(drive)) {
                    result.changed = true;
                    appendDiskIIDriveSettingUpdates(
                        updates, bus, *card, drive);
                } else {
                    result.failures.push_back(
                        "Disk II slot " + std::to_string(card->getSlot()) +
                        " drive " + std::to_string(drive + 1) + ": " +
                        card->getLastError(drive));
                }
            }
        }
        for (auto* card : cards.blockCards) {
            if (!card || !card->isImageLoaded()) continue;
            auto* peripheral = bus.peripheral(card->getSlot());
            if (card->ejectImage()) {
                result.changed = true;
                if (peripheral) {
                    (void)appendMediaBaySettingUpdates(
                        updates, *peripheral, card->getSlot(), 0,
                        autoHdvSlot_, autoSmartPortSlot_);
                }
            } else {
                result.failures.push_back(
                    "block device slot " + std::to_string(card->getSlot()) +
                    ": " + card->getLastError());
            }
        }
        for (auto* card : cards.smartPortCards) {
            if (!card) continue;
            for (std::size_t bay = 0; bay < SmartPortCard::kMaxUnits; ++bay) {
                auto* unit = card->unit(bay);
                if (!unit || !unit->isLoaded()) continue;
                if (unit->eject()) {
                    result.changed = true;
                    (void)appendMediaBaySettingUpdates(
                        updates, *card, card->getSlot(),
                        static_cast<int>(bay),
                        autoHdvSlot_, autoSmartPortSlot_);
                } else {
                    result.failures.push_back(
                        "SmartPort slot " +
                        std::to_string(card->getSlot()) + " bay " +
                        std::to_string(bay + 1) + ": " + unit->lastError());
                }
            }
        }
        onboardDisk35Loaded[0] = controller.disk35Internal().isLoaded();
        onboardDisk35Loaded[1] = controller.disk35External().isLoaded();
    }
    applySettingUpdates(settings, updates);
    if (!updates.empty()) (void)settings.save();

    // EmulationController::eject35 owns the same state lock internally, so
    // on-board media must be handled after the slot-media critical section.
    for (int drive = 0; drive < 2; ++drive) {
        if (!onboardDisk35Loaded[drive]) continue;
        if (controller.eject35(drive)) {
            result.changed = true;
            continue;
        }
        auto state = controller.lockState();
        const auto& image = drive == 0
            ? controller.disk35Internal() : controller.disk35External();
        result.failures.push_back(
            "on-board 3.5-inch drive " + std::to_string(drive + 1) +
            ": " + image.lastError());
    }
    return result;
}

StorageCoordinator::RestoreSettingsResult
StorageCoordinator::restoreMediaFromSettings(
    SlotBus& bus, const Settings& settings) const
{
    RestoreSettingsResult result;
    const auto cards = topology(bus);
    for (auto* card : cards.diskIICards) {
        if (!card) continue;
        const bool isPrimary = card == cards.primaryDiskII;
        card->setWriteBackEnabled(settings.getBool(
            "disk_writeback_slot" + std::to_string(card->getSlot()),
            isPrimary ? settings.getBool("disk_writeback", false) : false));
        for (std::size_t drive = 0; drive < kDiskIIDriveCount; ++drive) {
            const std::string path = settings.getString(
                diskIIPathSettingKey(card->getSlot(), drive),
                drive == 0 && isPrimary
                    ? settings.getString("disk_path", "") : std::string());
            std::error_code ec;
            if (!path.empty() && std::filesystem::is_regular_file(path, ec) &&
                !card->insertDisk(static_cast<int>(drive), path)) {
                result.warnings.push_back(
                    "Disk II slot " + std::to_string(card->getSlot()) +
                    " drive " + std::to_string(drive + 1) + ": " +
                    card->getLastError(static_cast<int>(drive)));
            }
        }
    }

    if (cards.primaryHdv) {
        const std::string path = settings.getString("hdv_path", "");
        std::error_code ec;
        if (!path.empty() && std::filesystem::is_regular_file(path, ec) &&
            !cards.primaryHdv->loadImage(path)) {
            result.warnings.push_back(
                "HDV slot " + std::to_string(cards.primaryHdv->getSlot()) +
                ": " + cards.primaryHdv->getLastError());
        }
        cards.primaryHdv->setWriteBackEnabled(
            settings.getBool("hdv_writeback", false));
    }

    for (auto* block : cards.blockCards) {
        auto* card = dynamic_cast<CffaCard*>(block);
        if (!card) continue;
        const std::string key =
            "cffa_slot" + std::to_string(card->getSlot());
        const std::string path = settings.getString(key + "_path", "");
        std::error_code ec;
        if (!path.empty() && std::filesystem::is_regular_file(path, ec) &&
            !card->loadImage(path)) {
            result.warnings.push_back(
                "CFFA slot " + std::to_string(card->getSlot()) + ": " +
                card->getLastError());
        }
        card->setWriteBackEnabled(
            settings.getBool(key + "_writeback", false));
    }

    for (auto* card : cards.smartPortCards) {
        if (!card) continue;
        const std::string slotKey =
            "smartport_slot" + std::to_string(card->getSlot());
        for (std::size_t bay = 0; bay < SmartPortCard::kMaxUnits; ++bay) {
            const std::string base =
                slotKey + "_unit" + std::to_string(bay);
            const std::string kind =
                settings.getString(base + "_type", "");
            if (kind.empty()) continue;
            auto unit = makeSmartPortUnit(kind);
            if (!unit) {
                result.warnings.push_back(
                    "SmartPort slot " + std::to_string(card->getSlot()) +
                    " bay " + std::to_string(bay + 1) +
                    ": unknown media type '" + kind + "'");
                continue;
            }
            unit->setWriteBackEnabled(
                settings.getBool(base + "_writeback", false));

            const std::string path =
                settings.getString(base + "_path", "");
            std::string resolved;
            if (!path.empty()) {
                std::error_code error;
                for (const std::string& candidate : {
                         path, std::string("../") + path,
                         std::string("../../") + path}) {
                    if (std::filesystem::is_regular_file(candidate, error)) {
                        resolved = candidate;
                        break;
                    }
                    error.clear();
                }
            }
            if (!resolved.empty() && !unit->loadImage(resolved)) {
                result.warnings.push_back(
                    "SmartPort slot " + std::to_string(card->getSlot()) +
                    " bay " + std::to_string(bay + 1) + ": " +
                    unit->lastError());
            } else if (resolved.empty() && !path.empty()) {
                result.warnings.push_back(
                    "SmartPort slot " + std::to_string(card->getSlot()) +
                    " bay " + std::to_string(bay + 1) +
                    ": persisted path not found: " + path);
            }
            card->setUnit(bay, std::move(unit));
        }
    }
    return result;
}

void StorageCoordinator::restoreRebuildSnapshot(
    SlotBus& bus, const RebuildSnapshot& snapshot) const
{
    for (const auto& disk : snapshot.diskII) {
        if (disk.slot < 1 || disk.slot >= SlotBus::kSlotCount) continue;
        auto* card = dynamic_cast<DiskIICard*>(bus.peripheral(disk.slot));
        if (!card) continue;

        // DiskImage snapshots the card policy during insert, so this order is
        // observable by the guest's write-protect probe.
        card->setWriteBackEnabled(disk.writeBackEnabled);
        for (std::size_t drive = 0; drive < disk.drives.size(); ++drive) {
            const auto& medium = disk.drives[drive];
            if (!medium.loaded || medium.path.empty()) {
                // The settings phase may have mounted a path last saved
                // before an in-session eject. A live snapshot entry proves
                // this card/drive existed and was empty, so it overrides that
                // stale setting. Cards absent from the snapshot are untouched.
                if (card->isDiskLoaded(static_cast<int>(drive)))
                    (void)card->ejectDisk(static_cast<int>(drive));
                continue;
            }
            std::error_code ec;
            if (std::filesystem::is_regular_file(medium.path, ec)) {
                (void)card->insertDisk(static_cast<int>(drive), medium.path);
            }
        }
    }

    const auto rebuilt = topology(bus);
    if (rebuilt.primaryHdv && snapshot.primaryHdv) {
        const auto& medium = *snapshot.primaryHdv;
        bool restored = false;
        if (medium.loaded && !medium.path.empty()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(medium.path, ec))
                restored = rebuilt.primaryHdv->loadImage(medium.path);
        }
        if (!restored && rebuilt.primaryHdv->isImageLoaded())
            (void)rebuilt.primaryHdv->ejectImage();
        rebuilt.primaryHdv->setWriteBackEnabled(medium.writeBackEnabled);
    }

    for (const auto& medium : snapshot.cffa) {
        if (medium.slot < 1 || medium.slot >= SlotBus::kSlotCount) {
            continue;
        }
        auto* card = dynamic_cast<CffaCard*>(bus.peripheral(medium.slot));
        if (!card) continue;
        bool restored = false;
        if (medium.loaded && !medium.path.empty()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(medium.path, ec))
                restored = card->loadImage(medium.path);
        }
        if (!restored && card->isImageLoaded()) (void)card->ejectImage();
        // Preserve the historical ordering for mounted CFFA images: apply
        // the opt-in after loading. Empty live state still owns the policy.
        card->setWriteBackEnabled(medium.writeBackEnabled);
    }
}

bool StorageCoordinator::flushAll(const SlotBus& bus,
                                  std::string& error) const
{
    error.clear();
    bool allSaved = true;
    const auto recordFailure = [&](std::string message) {
        if (!error.empty()) error += "; ";
        error += std::move(message);
        allSaved = false;
    };

    const auto cards = topology(bus);
    for (auto* card : cards.diskIICards) {
        if (card && !card->flushPendingWrites()) {
            recordFailure("Disk II slot " + std::to_string(card->getSlot()) +
                          ": " + card->getLastError());
        }
    }
    for (auto* card : cards.blockCards) {
        if (card && !card->saveDirty()) {
            recordFailure("block device slot " +
                          std::to_string(card->getSlot()) + ": " +
                          card->getLastError());
        }
    }
    for (auto* card : cards.smartPortCards) {
        for (size_t bay = 0; bay < SmartPortCard::kMaxUnits; ++bay) {
            auto* unit = card->unit(bay);
            if (unit && !unit->saveDirty()) {
                recordFailure("SmartPort slot " +
                              std::to_string(card->getSlot()) + " bay " +
                              std::to_string(bay + 1) + ": " +
                              unit->lastError());
            }
        }
    }
    return allSaved;
}

SmartPort_ImGui::CardSnapshot
StorageCoordinator::captureSmartPortPanel(
    EmulationController& controller) const
{
    SmartPort_ImGui::CardSnapshot snapshot;
    auto state = controller.lockState();
    auto& bus = state.memory().slotBus();
    const auto* card = smartPortAt(bus);
    if (!card) return snapshot;

    snapshot.plugged = true;
    snapshot.slot = card->getSlot();
    for (std::size_t unitIndex = 0;
         unitIndex < snapshot.units.size(); ++unitIndex) {
        const SmartPortUnit* unit = card->unit(unitIndex);
        auto& unitSnapshot = snapshot.units[unitIndex];
        if (!unit) continue;
        unitSnapshot.kind = std::string(unit->kindKey());
        unitSnapshot.kindLabel = std::string(unit->kindLabel());
        unitSnapshot.path = unit->path();
        unitSnapshot.lastError = unit->lastError();
        unitSnapshot.blockCount = unit->blockCount();
        unitSnapshot.loaded = unit->isLoaded();
        unitSnapshot.writeProtected = unit->isWriteProtected();
        unitSnapshot.writeBackEnabled = unit->isWriteBackEnabled();
    }
    return snapshot;
}

StorageCoordinator::PanelCommandStatus
StorageCoordinator::applySmartPortPanel(
    EmulationController& controller, Settings& settings, int slot,
    const SmartPort_ImGui::Result& command) const
{
    PanelCommandStatus status;
    std::vector<SettingUpdate> settingUpdates;
    const std::string slotKey = "smartport_slot" + std::to_string(slot);

    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        auto* card = smartPortAt(bus, slot);
        if (!card) return status;

        for (std::size_t unitIndex = 0;
             unitIndex < command.units.size(); ++unitIndex) {
            const auto& action = command.units[unitIndex];
            const std::string base = slotKey + "_unit" +
                                     std::to_string(unitIndex);
            const auto rememberString = [&](std::string key,
                                            std::string value) {
                settingUpdates.push_back(
                    {std::move(key), std::move(value), false, false});
            };
            const auto rememberBool = [&](std::string key, bool value) {
                settingUpdates.push_back(
                    {std::move(key), {}, value, true});
            };

            if (action.clearType || !action.setType.empty()) {
                if (action.clearType) {
                    card->setUnit(unitIndex, nullptr);
                    rememberString(base + "_type", "");
                    rememberString(base + "_path", "");
                    rememberBool(base + "_writeback", false);
                    status.message = "SmartPort unit " +
                        std::to_string(unitIndex) + ": cleared";
                } else {
                    auto unit = makeSmartPortUnit(action.setType);
                    if (unit) {
                        card->setUnit(unitIndex, std::move(unit));
                        rememberString(base + "_type", action.setType);
                        rememberString(base + "_path", "");
                        rememberBool(base + "_writeback", false);
                        status.message = "SmartPort unit " +
                            std::to_string(unitIndex) + ": type = " +
                            action.setType;
                    } else {
                        status.message = "SmartPort unit " +
                            std::to_string(unitIndex) + ": unknown type '" +
                            action.setType + "'";
                    }
                }
                status.visibleSeconds = 3.0;
                continue;
            }

            SmartPortUnit* unit = card->unit(unitIndex);
            if (!unit) continue;

            if (action.writeBackChanged) {
                unit->setWriteBackEnabled(action.writeBackOn);
                rememberBool(base + "_writeback", action.writeBackOn);
                status.message = "SmartPort unit " +
                    std::to_string(unitIndex) + ": write-back " +
                    (action.writeBackOn ? "ON" : "OFF");
                status.visibleSeconds = 3.0;
            }
            if (!action.mountPath.empty()) {
                if (unit->loadImage(action.mountPath)) {
                    rememberString(base + "_path", action.mountPath);
                    status.message = "SmartPort unit " +
                        std::to_string(unitIndex) + ": mounted " +
                        action.mountPath;
                } else {
                    status.message = "SmartPort unit " +
                        std::to_string(unitIndex) + ": mount failed: " +
                        unit->lastError();
                }
                status.visibleSeconds = 4.0;
            }
            if (action.eject) {
                const bool ok = unit->eject();
                if (ok) rememberString(base + "_path", "");
                status.message = "SmartPort unit " +
                    std::to_string(unitIndex) +
                    (ok ? ": ejected" : ": eject failed: " +
                                           unit->lastError());
                status.visibleSeconds = 4.0;
            }
        }
    }

    // Settings storage is deliberately outside the machine critical section.
    for (const auto& update : settingUpdates) {
        if (update.isBool)
            settings.setBool(update.key, update.boolValue);
        else
            settings.setString(update.key, update.stringValue);
    }
    if (!settingUpdates.empty()) settings.save();
    return status;
}

} // namespace pom2

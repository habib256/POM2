// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Additive, session-only slot provisioning for explicit boot intent. Unlike
// SlotRebuildCoordinator this never tears down a topology and never rewrites
// the user's effective slot plan.

#ifndef POM2_SLOT_PROVISIONING_COORDINATOR_H
#define POM2_SLOT_PROVISIONING_COORDINATOR_H

#include "SystemProfile.h"

#include <string>

class EmulationController;

namespace pom2 {

class Settings;
class SlotCardFactory;
class StorageCoordinator;

class SlotProvisioningCoordinator final
{
public:
    struct Result {
        int slot = -1;
        bool added = false;
        std::string error;

        explicit operator bool() const noexcept { return slot >= 0; }
    };

    SlotProvisioningCoordinator(SlotCardFactory& factory,
                                StorageCoordinator& storage);

    /// Return the bootable target selected by the active profile. If none is
    /// present, add a session-only HDV card in slot 7 (or highest free slot).
    Result ensureHdvBootTarget(EmulationController& controller,
                               const Settings& settings,
                               SystemProfile profile);

    /// Return the primary SmartPort target, or add a session-only Liron-class
    /// card in slot 5 (or highest free slot). No-physical-slot profiles cannot
    /// accept an expansion card and therefore fail when no built-in exists.
    Result ensureSmartPortBootTarget(EmulationController& controller,
                                     SystemProfile profile);

    bool isSessionOnlySlot(int slot) const noexcept;
    void resetSessionTracking() noexcept;

private:
    SlotCardFactory& factory_;
    StorageCoordinator& storage_;
};

} // namespace pom2

#endif // POM2_SLOT_PROVISIONING_COORDINATOR_H

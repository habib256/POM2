// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Owns the two configuration values which are not machine topology:
//
//   effectivePlan_  settings after profile/uniqueness policy
//   draft_          staged UI edits, never applied implicitly
//
// SlotBus remains the sole authority for what is actually plugged. A live
// snapshot is copied from it explicitly instead of mutating either map when a
// ROM is missing or a session-only card is auto-provisioned.

#ifndef POM2_SLOT_CONFIGURATION_COORDINATOR_H
#define POM2_SLOT_CONFIGURATION_COORDINATOR_H

#include "SystemProfile.h"

#include <array>
#include <cstddef>
#include <string>

class SlotBus;

namespace pom2 {

class Settings;

class SlotConfigurationCoordinator
{
public:
    using CardMap = std::array<std::string, 8>;

    struct LiveSnapshot {
        CardMap keys{};
        CardMap names{};

        bool plugged(int slot) const noexcept
        {
            return slot > 0 && slot < static_cast<int>(keys.size()) &&
                   !names[static_cast<std::size_t>(slot)].empty();
        }
    };

    /// Rebuild the effective plan from settings, then apply machine-profile
    /// fixtures and the single-instance policy. Also discards any stale draft.
    /// Index zero stays empty.
    const CardMap& resolve(const Settings& settings, SystemProfile profile);

    const CardMap& effectivePlan() const noexcept { return effectivePlan_; }

    /// Explicitly mutable because this is the staged editor value, not live
    /// hardware and not the effective plan used for construction.
    CardMap& draft() noexcept { return draft_; }
    const CardMap& draft() const noexcept { return draft_; }
    void resetDraft() { draft_ = effectivePlan_; }

    /// Copy the actual SlotBus topology. The caller must hold the machine
    /// state lock for the duration of this call.
    LiveSnapshot captureLive(const SlotBus& bus) const;

    static bool isMultiInstance(const std::string& cardKey) noexcept;

private:
    CardMap effectivePlan_{};
    CardMap draft_{};
};

} // namespace pom2

#endif // POM2_SLOT_CONFIGURATION_COORDINATOR_H

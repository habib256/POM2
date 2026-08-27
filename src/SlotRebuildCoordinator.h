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

// Transactional topology-rebuild sequencing. The caller still owns the
// profile- or settings-specific construction policy, but cannot clear the
// SlotBus before its external consumers have been detached or publish AI
// endpoints before the replacement topology is coherent.

#ifndef POM2_SLOT_REBUILD_COORDINATOR_H
#define POM2_SLOT_REBUILD_COORDINATOR_H

#include <cstdint>
#include <functional>

namespace pom2 {

class StateAccess;

class SlotRebuildCoordinator final
{
public:
    enum class Phase : std::uint8_t {
        Stable,
        Prepared,
        Rebuilding,
    };

    struct Hooks {
        // Runs only after every dirty medium has been flushed successfully.
        std::function<void()> invalidateHistoricalState;
        std::function<void()> detachControlEndpoints;
        std::function<void()> detachAudioSources;
        std::function<void()> detachFrontendViews;
        std::function<void()> resetPrinterCursor;
        std::function<void()> stopNetworkRuntime;
        std::function<void()> detachDisplayCard;
        std::function<void()> publishControlEndpoints;
    };

    explicit SlotRebuildCoordinator(Hooks hooks);

    SlotRebuildCoordinator(const SlotRebuildCoordinator&) = delete;
    SlotRebuildCoordinator& operator=(const SlotRebuildCoordinator&) = delete;

    /// Commit the rebuild after media durability has been established.
    /// Invalidates topology-bound history before any card is destroyed.
    void prepareAfterFlush();

    /// Detach every external consumer in dependency order, then clear the
    /// SlotBus. The StateAccess token proves the caller owns stateMutex.
    void beginLocked(const StateAccess& state);

    /// Publish the completed topology. Also requires stateMutex so no AI
    /// request can observe a partially published machine.
    void publishLocked(const StateAccess& state);

    Phase phase() const noexcept { return phase_; }
    std::uint64_t generation() const noexcept { return generation_; }

private:
    Hooks hooks_;
    Phase phase_ = Phase::Stable;
    std::uint64_t generation_ = 0;
};

} // namespace pom2

#endif // POM2_SLOT_REBUILD_COORDINATOR_H

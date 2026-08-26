// POM2 Apple II Emulator
// Copyright (C) 2026
//
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

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

#include "SlotRebuildCoordinator.h"

#include "EmulationController.h"
#include "Memory.h"
#include "SlotBus.h"

#include <stdexcept>
#include <utility>

namespace pom2 {
namespace {

void runHook(const std::function<void()>& callback)
{
    if (callback) callback();
}

} // namespace

SlotRebuildCoordinator::SlotRebuildCoordinator(Hooks hooks)
    : hooks_(std::move(hooks))
{
    if (!hooks_.invalidateHistoricalState ||
        !hooks_.detachControlEndpoints ||
        !hooks_.detachAudioSources ||
        !hooks_.detachFrontendViews ||
        !hooks_.resetPrinterCursor ||
        !hooks_.stopNetworkRuntime ||
        !hooks_.detachDisplayCard ||
        !hooks_.publishControlEndpoints) {
        throw std::invalid_argument(
            "slot rebuild coordinator requires every lifecycle hook");
    }
}

void SlotRebuildCoordinator::prepareAfterFlush()
{
    if (phase_ != Phase::Stable) {
        throw std::logic_error(
            "slot rebuild prepared while another transaction is active");
    }

    phase_ = Phase::Prepared;
    ++generation_;
    runHook(hooks_.invalidateHistoricalState);
}

void SlotRebuildCoordinator::beginLocked(const StateAccess& state)
{
    if (phase_ != Phase::Prepared) {
        throw std::logic_error(
            "slot rebuild teardown started before a successful flush");
    }

    // Gate new card-facing requests first. A request which already acquired
    // stateMutex completes against the still-live bus before this call.
    runHook(hooks_.detachControlEndpoints);

    // AudioDevice and panels retain non-owning views into card objects. They
    // must disappear before SlotBus releases ownership.
    runHook(hooks_.detachAudioSources);
    runHook(hooks_.detachFrontendViews);
    runHook(hooks_.resetPrinterCursor);

    state.memory().slotBus().clear();

    // These host-side services no longer have a card to represent. Preserve
    // the historical order: card/link teardown precedes helper shutdown.
    runHook(hooks_.stopNetworkRuntime);
    runHook(hooks_.detachDisplayCard);
    phase_ = Phase::Rebuilding;
}

void SlotRebuildCoordinator::publishLocked(const StateAccess& state)
{
    (void)state; // lock-ownership token; publication itself is host-side.
    if (phase_ != Phase::Rebuilding) {
        throw std::logic_error(
            "slot rebuild published before topology reconstruction");
    }

    // Publish last: every card, remounted medium and reset operation must be
    // coherent before external requests are allowed through again.
    runHook(hooks_.publishControlEndpoints);
    phase_ = Phase::Stable;
}

} // namespace pom2

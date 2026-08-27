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

// POM2 — Super Serial panel boundary.
//
// The SSC panel used to read eight fields and perform four mutations straight
// through a card pointer, unlocked, from a tab body. Two SSCs can be plugged
// at once (the //c's printer and modem ports), so "which card did that
// checkbox belong to" is a real question, and a slot rebuild between the tab
// rendering and the click landing is a real window.
//
// These cases pin the two guarantees the snapshot/command boundary buys:
// a command carries the slot it was raised for, and a command for a card that
// is no longer there is DROPPED rather than applied to whatever now occupies
// that slot.

#include "DevicePanelCoordinator.h"

#include "EmulationController.h"
#include "Memory.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {

using namespace pom2;

SuperSerialCard* plugSsc(EmulationController& controller, int slot)
{
    auto card = std::make_unique<SuperSerialCard>(slot);
    auto* raw = card.get();
    auto state = controller.lockState();
    state.memory().slotBus().plug(slot, std::move(card));
    return raw;
}

// ── The snapshot enumerates every plugged port, slot-ascending ───────────
void testSnapshotCoversBothPorts()
{
    EmulationController controller;
    Settings settings;
    DevicePanelCoordinator coordinator(controller, settings);

    plugSsc(controller, 1);
    plugSsc(controller, 2);

    const auto ports = coordinator.captureSerialCards();
    assert(ports.size() == 2);
    // Ascending, which is what makes the //c printer/modem tab labels right.
    assert(ports[0].slot == 1);
    assert(ports[1].slot == 2);

    std::printf("  snapshot covers both ports, slot-ascending: OK\n");
}

// ── A command is applied to the slot it names, not "the" card ────────────
void testCommandTargetsItsOwnSlot()
{
    EmulationController controller;
    Settings settings;
    DevicePanelCoordinator coordinator(controller, settings);

    auto* printerPort = plugSsc(controller, 1);
    auto* modemPort   = plugSsc(controller, 2);
    assert(!printerPort->rawMode() && !modemPort->rawMode());

    DevicePanelCoordinator::SerialCommand cmd;
    cmd.slot = 2;
    cmd.requestRawMode = true;
    cmd.rawMode = true;

    const auto result = coordinator.applySerial(cmd);
    assert(result.cardFound);
    assert(!printerPort->rawMode());   // the other tab is untouched
    assert(modemPort->rawMode());

    std::printf("  a command lands on the slot it names: OK\n");
}

// ── A command for a card that went away is dropped ───────────────────────
//
// This is the window the boundary exists to close: the tab rendered from a
// snapshot, the user clicked, and between those two moments a profile switch
// or a Slot Config Apply replaced the bus. Applying through a retained
// pointer would write to freed memory; applying by slot without checking
// would configure whatever card is there now.
void testCommandForRemovedCardIsDropped()
{
    EmulationController controller;
    Settings settings;
    DevicePanelCoordinator coordinator(controller, settings);

    plugSsc(controller, 2);
    const auto ports = coordinator.captureSerialCards();
    assert(ports.size() == 1 && ports[0].slot == 2);

    // The bus is rebuilt: slot 2 is now empty.
    {
        auto state = controller.lockState();
        state.memory().slotBus().clear();
    }

    DevicePanelCoordinator::SerialCommand cmd;
    cmd.slot = 2;
    cmd.requestRawMode = true;
    cmd.rawMode = true;

    const auto result = coordinator.applySerial(cmd);
    assert(!result.cardFound);   // dropped, not applied

    std::printf("  a command for a removed card is dropped: OK\n");
}

// ── An empty command does nothing at all ─────────────────────────────────
//
// The panel builds one command per frame and applies it unconditionally, so
// "no button was pressed" must be free and must not re-resolve anything.
void testEmptyCommandIsInert()
{
    EmulationController controller;
    Settings settings;
    DevicePanelCoordinator coordinator(controller, settings);

    auto* card = plugSsc(controller, 2);
    card->setRawMode(true);

    DevicePanelCoordinator::SerialCommand cmd;
    cmd.slot = 2;
    assert(cmd.empty());

    const auto result = coordinator.applySerial(cmd);
    assert(!result.cardFound);      // never looked
    assert(card->rawMode());        // and changed nothing

    std::printf("  an empty command is inert: OK\n");
}

} // namespace

int main()
{
    std::printf("Super Serial panel boundary\n");
    testSnapshotCoversBothPorts();
    testCommandTargetsItsOwnSlot();
    testCommandForRemovedCardIsDropped();
    testEmptyCommandIsInert();
    std::printf("OK\n");
    return 0;
}

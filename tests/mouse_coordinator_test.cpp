// MouseCoordinator topology, snapshot and lifetime contract.

#include "EmulationController.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "MouseCoordinator.h"
#include "PrinterCard.h"
#include "SlotBus.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>

int main()
{
    EmulationController controller;
    pom2::MouseCoordinator mouse(controller);

    {
        auto state = controller.lockState();
        auto& memory = state.memory();
        auto& bus = memory.slotBus();
        bus.plug(2, std::make_unique<MouseCard>(2));
        bus.plug(5, std::make_unique<MouseCardAppleWin>(5));

        memory.memWrite(0x047D, 0x34);
        memory.memWrite(0x057D, 0x12);
        memory.memWrite(0x04FD, 0x78);
        memory.memWrite(0x05FD, 0x56);
        memory.memWrite(0x077D, 0xA5);
        memory.memWrite(0x07FD, 0x5A);
    }

    const auto both = mouse.capture();
    assert(both.mamePlugged);
    assert(both.appleWinPlugged);
    assert(both.plugged());
    assert(both.appleWinActive());
    assert(both.slot == 5);
    assert(both.holes.x() == 0x1234);
    assert(both.holes.y() == 0x5678);
    assert(both.holes.status == 0xA5);
    assert(both.holes.mode == 0x5A);
    assert(mouse.routeHost(23, 197, true) == 2);

    // Destroy the previously selected card and replace it with an unrelated
    // device. The next operation must resolve fresh topology, never dereference
    // a retained alias.
    {
        auto state = controller.lockState();
        auto& bus = state.memory().slotBus();
        (void)bus.unplug(5);
        bus.plug(5, std::make_unique<PrinterCard>(5));
    }
    const auto mameOnly = mouse.capture();
    assert(mameOnly.kind == pom2::MouseCoordinator::Kind::Mame);
    assert(mameOnly.slot == 2);
    assert(mameOnly.mamePlugged && !mameOnly.appleWinPlugged);
    assert(mouse.routeHost(1, 2, false) == 1);

    {
        auto state = controller.lockState();
        (void)state.memory().slotBus().unplug(2);
    }
    const auto none = mouse.capture();
    assert(!none.plugged());
    assert(none.kind == pom2::MouseCoordinator::Kind::None);
    assert(none.slot == -1);
    assert(mouse.routeHost(0, 0, false) == 0);

    std::cout << "mouse coordinator: OK\n";
    return 0;
}

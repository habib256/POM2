// POM2 Apple II Emulator
// Copyright (C) 2026

#include "MouseCoordinator.h"

#include "EmulationController.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "SlotBus.h"

namespace pom2 {
namespace {

void copyAppleWin(MouseCoordinator::AppleWinSnapshot& out,
                  const MouseCardAppleWin::DebugSnapshot& in)
{
    out.iX = in.iX;
    out.iY = in.iY;
    out.nX = in.nX;
    out.nY = in.nY;
    out.iMinX = in.iMinX;
    out.iMaxX = in.iMaxX;
    out.iMinY = in.iMinY;
    out.iMaxY = in.iMaxY;
    out.bBtn0 = in.bBtn0;
    out.bBtn1 = in.bBtn1;
    out.bPrevBtn0 = in.bPrevBtn0;
    out.bPrevBtn1 = in.bPrevBtn1;
    out.byMode = in.byMode;
    out.byState = in.byState;
    out.by6821A = in.by6821A;
    out.by6821B = in.by6821B;
    out.buffPos = in.buffPos;
    out.dataLen = in.dataLen;
    out.lastCmd = in.lastCmd;
}

} // namespace

MouseCoordinator::Snapshot MouseCoordinator::capture() const
{
    Snapshot snapshot;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();

    MouseCard* mame = nullptr;
    MouseCardAppleWin* appleWin = nullptr;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (auto* mameCard = dynamic_cast<MouseCard*>(peripheral)) {
            snapshot.mamePlugged = true;
            mame = mameCard;
        } else if (auto* appleWinCard =
                       dynamic_cast<MouseCardAppleWin*>(peripheral)) {
            snapshot.appleWinPlugged = true;
            appleWin = appleWinCard;
        }
    }

    if (appleWin) {
        snapshot.kind = Kind::AppleWin;
        snapshot.slot = appleWin->getSlot();
        copyAppleWin(snapshot.appleWin, appleWin->debugSnapshot());
    } else if (mame) {
        snapshot.kind = Kind::Mame;
        snapshot.slot = mame->getSlot();
    }

    if (snapshot.slot > 0 && snapshot.slot < SlotBus::kSlotCount) {
        auto& memory = state.memory();
        snapshot.holes.xLo = memory.peekMainRam(
            static_cast<std::uint16_t>(0x0478 + snapshot.slot));
        snapshot.holes.xHi = memory.peekMainRam(
            static_cast<std::uint16_t>(0x0578 + snapshot.slot));
        snapshot.holes.yLo = memory.peekMainRam(
            static_cast<std::uint16_t>(0x04F8 + snapshot.slot));
        snapshot.holes.yHi = memory.peekMainRam(
            static_cast<std::uint16_t>(0x05F8 + snapshot.slot));
        snapshot.holes.status = memory.peekMainRam(
            static_cast<std::uint16_t>(0x0778 + snapshot.slot));
        snapshot.holes.mode = memory.peekMainRam(
            static_cast<std::uint16_t>(0x07F8 + snapshot.slot));
    }
    return snapshot;
}

int MouseCoordinator::routeHost(std::uint8_t rawX, std::uint8_t rawY,
                                bool button)
{
    int routed = 0;
    auto state = controller_.lockState();
    auto& bus = state.memory().slotBus();
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        auto* peripheral = bus.peripheral(slot);
        if (auto* mameCard = dynamic_cast<MouseCard*>(peripheral)) {
            mameCard->setHostMouse(rawX, rawY, button);
            ++routed;
        } else if (auto* appleWinCard =
                       dynamic_cast<MouseCardAppleWin*>(peripheral)) {
            appleWinCard->setHostMouse(rawX, rawY, button);
            ++routed;
        }
    }
    return routed;
}

} // namespace pom2

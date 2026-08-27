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

#pragma once

// Open-Apple / Solid-Apple latch arbitration — who gets to press $C061/$C062.
//
// One concern: POM2 has TWO things pressing the same two wires, and each must
// be able to press and release without disturbing the other.
//
//   • the host keyboard's Left / Right Alt, edged in `MainWindow::onKey`;
//   • the on-screen Apple //e keyboard's OPEN-APPLE / SOLID-APPLE latches,
//     which the panel republishes every frame because the Apple keys are
//     LEVELS, not events (the firmware reads bit 7 of the switch, so a level
//     has to be held for as long as the key is down).
//
// The bug this exists to prevent: each source used to assign the shared latch
// directly. The panel runs every frame, so it stamped whatever the host had
// just set back to its own value 60x/s — and because `keyboardPanel` is never
// destroyed once built, the panel's *closed* branch went on clearing both
// wires for the rest of the session. Opening the keyboard window once and
// closing it silently disabled Left/Right Alt: Open-Apple+Ctrl+Reset stopped
// cold-booting, and every //e title that reads $C061/$C062 bit 7 as button
// 0/1 stopped seeing the keys. It read as an emulator fault rather than a UI
// one, because `Memory::memRead` ORs the paddle buttons in on the same case,
// so a real joystick kept working.
//
// The fix is structural, not a patch on the ordering: sources are held apart
// and OR'd at the point of use, so no writer can express "and release the
// other one". Header-only and emulator-free, so the rules stay pinnable by a
// headless test (`tests/apple_key_latch_test.cpp`) that links no windowing
// stack — same contract as `MouseGrab.h`.

namespace pom2 {

/// The two Apple keys, tracked per source. `openApple()` / `solidApple()` are
/// what reaches `Memory::setOpenAppleKey` / `setSolidAppleKey`.
struct AppleKeyLatch {
    // Host Left Alt / Right Alt. Edged: press sets, release clears.
    bool hostOpen   = false;
    bool hostSolid  = false;
    // The on-screen keyboard's latches. Republished every frame while its
    // window is open, dropped when it closes.
    bool panelOpen  = false;
    bool panelSolid = false;

    /// Either source alone is enough to hold the wire down — a mouse has one
    /// pointer, so clicking OPEN-APPLE on the picture and holding Alt on the
    /// host are both legitimate ways to get the same bit set, and a user may
    /// well combine them (latch Open-Apple, then hold Ctrl on the host).
    constexpr bool openApple()  const { return hostOpen  || panelOpen;  }
    constexpr bool solidApple() const { return hostSolid || panelSolid; }

    /// Host Alt edge. Touches only the host half.
    constexpr void setHost(bool open, bool solid) {
        hostOpen = open;  hostSolid = solid;
    }

    /// The on-screen keyboard's latches for this frame. Touches only the
    /// panel half — this is the call that used to clear the host's Alt.
    constexpr void setPanel(bool open, bool solid) {
        panelOpen = open; panelSolid = solid;
    }

    /// The keyboard window closed: its latches are gone with the UI that
    /// showed them as down (nothing left to un-latch them with). The host's
    /// Alt is NOT a casualty of that — it has its own release edge.
    constexpr void releasePanel() { panelOpen = false; panelSolid = false; }
};

} // namespace pom2

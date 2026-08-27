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

// Open-Apple / Solid-Apple source arbitration — pins src/AppleKeyLatch.h.
//
// $C061/$C062 bit 7 is a single wire with two things pressing it: the host's
// Left/Right Alt and the on-screen Apple //e keyboard's latches. What this
// pins is the one property that makes that safe — the sources are
// INDEPENDENT. Neither writer can express "…and release the other one".
//
// The regression it exists for: both sources used to assign the shared latch
// directly. The keyboard panel republishes its latches every frame (the Apple
// keys are levels, not events), so it stamped the host's Alt back to false
// 60x/s; and since `keyboardPanel` is never destroyed once built, the panel's
// *closed* branch went on clearing both wires for the rest of the session.
// Opening the keyboard window once and closing it therefore disabled
// Left/Right Alt for good: Open-Apple+Ctrl+Reset stopped cold-booting and
// every //e title reading bit 7 as button 0/1 stopped seeing the keys. It
// looked like an emulator fault rather than a UI one, because `Memory` ORs
// the paddle buttons into the same case — so a real joystick kept working.
//
// AppleKeyLatch.h is emulator-free, so this links no windowing stack.

#include "AppleKeyLatch.h"

#include <cassert>
#include <cstdio>

using pom2::AppleKeyLatch;

namespace {

// Either source alone holds the wire down: a mouse has one pointer, so
// clicking OPEN-APPLE on the picture and holding Alt on the host are both
// legitimate ways to set the same bit.
void testEitherSourceAloneDrivesTheWire()
{
    AppleKeyLatch k;
    assert(!k.openApple() && !k.solidApple());

    k.setHost(true, false);
    assert(k.openApple() && !k.solidApple());
    k.setHost(false, true);
    assert(!k.openApple() && k.solidApple());

    k = AppleKeyLatch{};
    k.setPanel(true, false);
    assert(k.openApple() && !k.solidApple());
    k.setPanel(false, true);
    assert(!k.openApple() && k.solidApple());
}

// THE regression. The panel publishes every frame; that must not disturb a
// held Alt. Repeated to make the point that it is the *steady state* — one
// frame of coincidence would not have hidden the original bug.
void testPanelRepublishNeverClearsHostAlt()
{
    AppleKeyLatch k;
    k.setHost(/*open=*/true, /*solid=*/true);       // user holds both Alts

    for (int frame = 0; frame < 120; ++frame) {
        k.setPanel(false, false);                   // panel: nothing latched
        assert(k.openApple()  && "host Left Alt survives the panel's frame");
        assert(k.solidApple() && "host Right Alt survives the panel's frame");
    }
    // …and the host's own release edge still works.
    k.setHost(false, false);
    assert(!k.openApple() && !k.solidApple());
}

// The other half of the regression: closing the window drops the PANEL's
// latches only. `releasePanel` is what the closed branch calls, and it ran
// every frame for the rest of the session — harmless now precisely because it
// cannot reach the host half.
void testClosingThePanelLeavesHostAltAlone()
{
    AppleKeyLatch k;
    k.setHost(true, true);
    k.setPanel(true, true);
    assert(k.openApple() && k.solidApple());

    for (int frame = 0; frame < 120; ++frame) {
        k.releasePanel();
        assert(k.openApple()  && "closing the keyboard window is not an Alt release");
        assert(k.solidApple() && "closing the keyboard window is not an Alt release");
    }
    assert(!k.panelOpen && !k.panelSolid && "the panel half really did drop");
}

// Symmetry: the host's release edge must not take the panel's latch with it
// either. Latching OPEN-APPLE on the picture and then tapping host Alt is a
// real gesture — it is how Open-Apple+Ctrl+Reset gets clicked one key at a
// time, which is the whole reason the panel latches instead of chording.
void testHostReleaseLeavesPanelLatchAlone()
{
    AppleKeyLatch k;
    k.setPanel(true, true);
    k.setHost(true, true);
    assert(k.openApple() && k.solidApple());

    k.setHost(false, false);                        // user lifts both Alts
    assert(k.openApple()  && "the panel's OPEN-APPLE latch is still down");
    assert(k.solidApple() && "the panel's SOLID-APPLE latch is still down");

    k.releasePanel();                               // only now does it lift
    assert(!k.openApple() && !k.solidApple());
}

// The two keys are separate wires: pressing one must never move the other.
// A crossed assignment here would be invisible in normal use (both are
// usually read as "some button") right up until a title distinguishes them.
void testTheTwoKeysAreIndependent()
{
    AppleKeyLatch k;
    k.setHost(true, false);
    k.setPanel(false, false);
    assert(k.openApple() && !k.solidApple());

    k.setPanel(false, true);
    assert(k.openApple() && k.solidApple());

    k.setHost(false, false);
    assert(!k.openApple() && k.solidApple() && "Solid-Apple outlives Open-Apple");
}

} // namespace

int main()
{
    testEitherSourceAloneDrivesTheWire();
    testPanelRepublishNeverClearsHostAlt();
    testClosingThePanelLeavesHostAltAlone();
    testHostReleaseLeavesPanelLatchAlone();
    testTheTwoKeysAreIndependent();
    std::printf("apple_key_latch: all tests passed\n");
    return 0;
}

// Mouse Card pointer-capture policy — pins src/MouseGrab.h.
//
// The grab is the one host-input mode that can leave a user with no visible
// cursor and no obvious way back, so what this pins is exactly the set of
// rules that keep it escapable and honest:
//
//   • the release chord and the release button really do match (and match
//     nothing else) — a typo here strands the pointer;
//   • the capturing click is distinguishable from a normal click, because
//     the caller must SWALLOW the former (the guest cursor is not under the
//     host pointer, so forwarding it would click at an arbitrary spot);
//   • a button RELEASE always reaches the card, from anywhere, so the guest
//     can never be left holding a button down;
//   • capture bypasses the widget-rect gate on motion (that gate is the
//     whole reason to capture: it is what makes the guest cursor unable to
//     reach its own clamp edges) while a non-captured pointer keeps the
//     pre-grab contract;
//   • absolute closed-loop sync is off while captured — under
//     GLFW_CURSOR_DISABLED the reported position is virtual and maps to no
//     point on screen.
//
// MouseGrab.h stays GLFW-free so this test links no windowing stack; the
// mirrored GLFW token values are re-asserted here, and MainWindow.cpp
// static_asserts them against the real <GLFW/glfw3.h>.

#include "MouseGrab.h"

#include <cassert>
#include <cstdio>

namespace mg = pom2::mousegrab;

namespace {

// A Context with a Mouse Card plugged and the cursor over the screen —
// the state every "does this fire?" case starts from.
mg::Context hovering()
{
    mg::Context c;
    c.cardPlugged  = true;
    c.insideScreen = true;
    return c;
}

void testToggleChord()
{
    // GLFW folds left and right Alt into one modifier bit, so one test
    // covers both; extra modifiers must not break the escape hatch.
    assert(mg::isToggleChord(mg::kKeyG, mg::kModControl | mg::kModAlt));
    assert(mg::isToggleChord(mg::kKeyG,
                             mg::kModControl | mg::kModAlt | 0x0001 /*shift*/));
    assert(mg::isToggleChord(mg::kKeyG,
                             mg::kModControl | mg::kModAlt | 0x0008 /*super*/));

    // Neither half alone: plain Ctrl-G must keep reaching the Apple II
    // keyboard latch as $07 (BELL), and Alt-G is Open-Apple + G.
    assert(!mg::isToggleChord(mg::kKeyG, mg::kModControl));
    assert(!mg::isToggleChord(mg::kKeyG, mg::kModAlt));
    assert(!mg::isToggleChord(mg::kKeyG, 0));
    // Not some neighbouring key with the right modifiers.
    assert(!mg::isToggleChord(mg::kKeyG + 1, mg::kModControl | mg::kModAlt));
    assert(!mg::isToggleChord(mg::kKeyG - 1, mg::kModControl | mg::kModAlt));
}

void testReleaseButton()
{
    assert(mg::isReleaseButton(mg::kButtonMiddle));
    assert(!mg::isReleaseButton(mg::kButtonLeft));
    assert(!mg::isReleaseButton(1));   // right
    assert(!mg::isReleaseButton(3));   // thumb buttons and beyond
}

void testGrabOnPress()
{
    mg::Context c = hovering();
    assert(mg::shouldGrabOnPress(c, mg::kButtonLeft));

    // Only the left button ever captures — middle is release-only (it would
    // collide with the 3D voxel view's middle-drag pan), right does nothing.
    assert(!mg::shouldGrabOnPress(c, mg::kButtonMiddle));
    assert(!mg::shouldGrabOnPress(c, 1));

    // Every gate, one at a time.
    mg::Context off = c; off.clickToGrab = false;
    assert(!mg::shouldGrabOnPress(off, mg::kButtonLeft));
    mg::Context already = c; already.grabbed = true;
    assert(!mg::shouldGrabOnPress(already, mg::kButtonLeft));
    mg::Context noCard = c; noCard.cardPlugged = false;
    assert(!mg::shouldGrabOnPress(noCard, mg::kButtonLeft));
    mg::Context outside = c; outside.insideScreen = false;
    assert(!mg::shouldGrabOnPress(outside, mg::kButtonLeft));
    // The voxel view owns left-drag (orbit) and middle-drag (pan); capturing
    // would swallow both. Ctrl+Alt+G still works there.
    mg::Context voxel = c; voxel.voxelView = true;
    assert(!mg::shouldGrabOnPress(voxel, mg::kButtonLeft));
}

void testMotionRouting()
{
    mg::Context c = hovering();
    assert(mg::shouldRouteMotion(c));

    // Uncaptured and off the widget: ImGui's, so the user can still reach
    // the menus and panels.
    mg::Context outside = c; outside.insideScreen = false;
    assert(!mg::shouldRouteMotion(outside));

    // Captured: the widget rect stops meaning anything — this is the whole
    // point of the mode.
    mg::Context grabbedOutside = outside; grabbedOutside.grabbed = true;
    assert(mg::shouldRouteMotion(grabbedOutside));

    // ... but never with no card to feed.
    mg::Context noCard = grabbedOutside; noCard.cardPlugged = false;
    assert(!mg::shouldRouteMotion(noCard));
}

void testButtonRouting()
{
    mg::Context c = hovering();
    assert(mg::shouldRouteButton(c, mg::kButtonLeft, /*press=*/true));

    // Only the primary button is wired (PB7 of the MCU).
    assert(!mg::shouldRouteButton(c, mg::kButtonMiddle, true));
    assert(!mg::shouldRouteButton(c, 1, true));
    assert(!mg::shouldRouteButton(c, 1, false));

    // A press outside the widget belongs to ImGui...
    mg::Context outside = c; outside.insideScreen = false;
    assert(!mg::shouldRouteButton(outside, mg::kButtonLeft, true));
    // ...but its RELEASE still passes through: pressed inside, released
    // outside must not leave the button stuck down in the guest.
    assert(mg::shouldRouteButton(outside, mg::kButtonLeft, false));

    // Captured: every press is the guest's, wherever the virtual cursor is.
    mg::Context grabbedOutside = outside; grabbedOutside.grabbed = true;
    assert(mg::shouldRouteButton(grabbedOutside, mg::kButtonLeft, true));

    // No card, nothing routed — not even a release.
    mg::Context noCard = c; noCard.cardPlugged = false;
    assert(!mg::shouldRouteButton(noCard, mg::kButtonLeft, true));
    assert(!mg::shouldRouteButton(noCard, mg::kButtonLeft, false));
}

void testAbsoluteSyncGate()
{
    mg::Context c = hovering();
    assert(mg::allowAbsoluteSync(c));
    mg::Context grabbed = c; grabbed.grabbed = true;
    assert(!mg::allowAbsoluteSync(grabbed));
}

void testGlfwTokenMirrors()
{
    // MouseGrab.h cannot include <GLFW/glfw3.h> (this test links no GLFW),
    // so the values are mirrored there. MainWindow.cpp static_asserts them
    // against the real header; this catches an edit to the mirrors alone.
    static_assert(mg::kKeyG         == 71);
    static_assert(mg::kModControl   == 0x0002);
    static_assert(mg::kModAlt       == 0x0004);
    static_assert(mg::kButtonLeft   == 0);
    static_assert(mg::kButtonMiddle == 2);
}

} // namespace

int main()
{
    testToggleChord();
    testReleaseButton();
    testGrabOnPress();
    testMotionRouting();
    testButtonRouting();
    testAbsoluteSyncGate();
    testGlfwTokenMirrors();
    std::printf("OK\n");
    return 0;
}

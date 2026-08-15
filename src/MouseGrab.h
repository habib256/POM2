#pragma once

// Host-pointer capture ("mouse grab") policy for the Apple II Mouse Card.
//
// One concern: *deciding* what a host input event means while the pointer is
// (or isn't) captured. Actually flipping GLFW's cursor mode, hiding the OS
// cursor and feeding the card lives in MainWindow — this header only answers
// the questions, so the decisions stay pinnable by a headless test that links
// no windowing stack.
//
// Why grab at all? The Mouse Card (both variants) is a purely *relative*
// quadrature device: MainWindow converts host-pixel deltas into Apple mouse
// units and the guest firmware clamps them at its own window edges. Without a
// grab, the host pointer hits the edge of the Apple II Screen widget (or of
// the desktop) long before the guest cursor reaches the edge of its clamp
// window, and every further delta is lost — the two cursors desynchronise and
// the guest cursor can become unable to reach a menu bar or a scroll gutter.
// Capturing the pointer (GLFW_CURSOR_DISABLED) removes the boundary entirely:
// GLFW reports an unbounded virtual position, so deltas keep flowing in every
// direction for as long as the user keeps moving.
//
// The escape hatch is deliberately the one every VM viewer and PC emulator
// uses, so it is already in the user's fingers: **Ctrl+Alt+G**, or a click on
// the **middle button (wheel)**.

namespace pom2 {
namespace mousegrab {

// Mirrors of the GLFW tokens this policy compares against. Kept as literals
// so the header stays GLFW-free (tests link it without GLFW/OpenGL);
// MainWindow.cpp static_asserts every one of them against <GLFW/glfw3.h>, so
// a token change upstream is a compile error rather than a silent desync.
inline constexpr int kKeyG         = 71;      // GLFW_KEY_G
inline constexpr int kModControl   = 0x0002;  // GLFW_MOD_CONTROL
inline constexpr int kModAlt       = 0x0004;  // GLFW_MOD_ALT
inline constexpr int kButtonLeft   = 0;       // GLFW_MOUSE_BUTTON_LEFT
inline constexpr int kButtonMiddle = 2;       // GLFW_MOUSE_BUTTON_MIDDLE

/// Ctrl+Alt+G — grab/release toggle. Matched on either Alt (GLFW folds
/// left and right Alt into the same GLFW_MOD_ALT bit) and regardless of
/// Shift/Super, so a stray modifier can never strand a captured pointer.
/// Must be tested BEFORE the Ctrl-letter path that injects $01..$1A into
/// the Apple II keyboard latch, or the chord would also type Ctrl-G ($07).
inline bool isToggleChord(int key, int mods)
{
    return key == kKeyG && (mods & kModControl) != 0 && (mods & kModAlt) != 0;
}

/// Middle button (wheel click) — release only, never grabs. Entering on a
/// middle click would collide with the 3D voxel view's middle-drag pan, and
/// an escape hatch that can also *arm* the trap is a worse escape hatch.
inline bool isReleaseButton(int button) { return button == kButtonMiddle; }

/// Everything the policy needs to know about the current host/UI state.
struct Context {
    bool cardPlugged  = false;  ///< MouseCard or MouseCardAppleWin on the bus
    bool grabbed      = false;  ///< host pointer currently captured
    bool insideScreen = false;  ///< host cursor within the Apple II Screen rect
    bool voxelView    = false;  ///< 3D voxel view owns the drag gestures
    bool clickToGrab  = true;   ///< user preference (mouse_click_to_grab)
};

/// A left press inside the screen widget captures the pointer — the classic
/// "click the screen to give the guest your mouse" contract.
///
/// The caller must test this BEFORE `shouldRouteButton` and swallow the press
/// when it returns true: the guest cursor is wherever its firmware left it,
/// not under the host pointer, so forwarding the capturing click would fire a
/// button-down at an arbitrary spot (a stray dot in MousePaint, a wrong menu
/// pick in A2Desktop). Every press after the grab does reach the card.
///
/// Suppressed while the 3D voxel view is up: there, left-drag orbits the
/// camera and grabbing would swallow the gesture. Ctrl+Alt+G still works.
inline bool shouldGrabOnPress(const Context& c, int button)
{
    return button == kButtonLeft && c.clickToGrab && !c.grabbed &&
           c.cardPlugged && c.insideScreen && !c.voxelView;
}

/// Motion routing. Grabbed: every delta is the guest's, wherever GLFW's
/// virtual cursor has wandered to — the widget rect is meaningless once the
/// pointer is captured. Not grabbed: the pre-grab contract, i.e. only motion
/// over the screen widget, so the host can still drive ImGui panels.
inline bool shouldRouteMotion(const Context& c)
{
    return c.cardPlugged && (c.grabbed || c.insideScreen);
}

/// Button routing to the card. Only the primary button is wired (PB7 of the
/// MCU). A release ALWAYS passes through even from outside the widget, so a
/// button pressed inside the screen and released elsewhere still clears on
/// the card instead of sticking down in the guest.
inline bool shouldRouteButton(const Context& c, int button, bool press)
{
    if (button != kButtonLeft || !c.cardPlugged) return false;
    if (!press)                                  return true;
    return c.grabbed || c.insideScreen;
}

/// The AppleWin HLE card's absolute closed-loop sync projects the host
/// cursor's position-in-widget onto the firmware clamp window. Under a grab
/// GLFW reports an unbounded virtual position that no longer corresponds to
/// any point on screen, so that projection is meaningless — a grabbed pointer
/// always drives the relative path.
inline bool allowAbsoluteSync(const Context& c) { return !c.grabbed; }

} // namespace mousegrab
} // namespace pom2

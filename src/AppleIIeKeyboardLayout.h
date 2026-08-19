// VERHILLE Arnaud 2026
//
// POM2 Apple II Emulator
// Copyright (C) 2026
//
// AppleIIeKeyboardLayout — where every key sits on `pic/Keyboard_AppleIIe.jpeg`
// and what it sends.
//
// GENERATED, and worth saying how, because the numbers are not guesses:
// `tools/gen_keyboard_layout.py` reads the photo, takes a 75th-percentile
// column profile through the middle of each key row (75th, not the median:
// the two-line French/US legends put enough dark glyph pixels in the middle
// of a cap to split it in two under a median) and cuts at the dark valleys
// between caps. That yields 5 row bands and the cap runs inside each, which
// this file stores as fractions of the 2578x908 image — so the hotspots
// track the picture at any window size, and re-cropping the photo means
// re-running the script rather than nudging constants by hand.
//
// The photo is a EUROPEAN (French) //e keyboard: every alphanumeric cap
// carries two legends, French on top and US below ("A Q" is French A / US Q).
// POM2 maps the US legend, which is what the emulated ROM's keyboard decoder
// expects. Two consequences worth knowing before reading the table:
//   * the extra ISO key at the left of row 4 (legends "> |" / "< \\") is the
//     US backslash key;
//   * the photo draws BOTH horizontal arrow caps pointing left. That is an
//     error in the picture, not a layout: the //e has left then right, and
//     the table follows the hardware. The tooltip names each one.
//
// The empty recess between Caps Lock and Open-Apple is left out on purpose —
// it is a bare well on the real machine, with no cap and no switch.

#ifndef POM2_APPLE_IIE_KEYBOARD_LAYOUT_H
#define POM2_APPLE_IIE_KEYBOARD_LAYOUT_H

#include <vector>

namespace pom2 {

/// What a hotspot does when clicked.
enum class KeyKind {
    Char,      ///< Injects `base`, or `shift` when Shift is latched.
    Special,   ///< A named action (Return, arrows, Reset...).
    Modifier   ///< A latch (Shift / Control / Caps / Open- and Solid-Apple).
};

/// Named actions and latches. `Char` keys use `base`/`shift` instead.
enum class KeyAction {
    None, Esc, Tab, Return, Delete, Left, Right, Up, Down, Reset,
    Shift, Control, CapsLock, OpenApple, SolidApple
};

/// One clickable cap. Rect is in IMAGE FRACTIONS (0..1), x0/y0 top-left.
/// Two entries may share an `id` — the Return key is L-shaped and is stored
/// as its two arms, which is also how it highlights as one key.
struct KeyHotspot {
    const char* id;
    const char* label;     ///< Tooltip / status text.
    KeyKind     kind;
    KeyAction   action;    ///< Meaningful for Special and Modifier.
    char        base;      ///< Char keys: unshifted ASCII.
    char        shift;     ///< Char keys: shifted ASCII.
    float       x0, y0, x1, y1;
};

/// The layout. Row order, left to right.
const std::vector<KeyHotspot>& appleIIeKeyboard();

} // namespace pom2

#endif // POM2_APPLE_IIE_KEYBOARD_LAYOUT_H

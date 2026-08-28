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

// PanelRegistry — where a panel's visibility LIVES, and what it can do now.
//
// `PanelCatalog.h` says what a panel is (id, title, menu group, settings key,
// tooltip). This holds the two things that only exist while the emulator is
// running: whether the panel is open, and the callbacks that depend on the
// live machine — is its card plugged (greys the row), what does its label read
// when the label carries a slot number, and how does it draw.
//
// Everything the UI used to keep as a parallel list is a view of this:
//
//   visible(PanelId::Debugger)          // was `bool showDebugger`, ×40 members
//   for (const PanelInfo& p : catalog)  // the menus, the palette
//   forEachPersisted(...)               // settings load + save
//   drawAll()                           // was 43 renderXxxWindow() calls
//   hideAll()                           // the WASM chrome-light block
//
// ── Ownership ────────────────────────────────────────────────────────────
// The registry owns the flags, in one `std::array<bool, PanelId::Count>`
// indexed by the enumerator. `MainWindow` kept them as ~40 `bool showXxx`
// members until 2026-08-23; `show(PanelId::X)` is the same `bool&`, reachable
// by a name the compiler checks against the catalog.
//
// ── Threading ────────────────────────────────────────────────────────────
// UI thread only. A panel's visibility is not emulator state.

#ifndef POM2_PANEL_REGISTRY_H
#define POM2_PANEL_REGISTRY_H

#include "PanelCatalog.h"

#include <array>
#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class PanelRegistry {
public:
    /// What a panel can do that only the running machine knows.
    /// Every member carries a default initialiser, including the three
    /// std::functions that obviously default to empty. That is not
    /// decoration: callers brace-initialise only the fields they mean
    /// (`RT{ title }`, `RT{ title, available }`), and without the defaults
    /// each of those is a -Wmissing-field-initializers warning — nine of
    /// them, drowning out the real ones on the way to -Werror.
    struct Runtime {
        /// Runtime label, when the static title is not the whole story (a slot
        /// number, "no card plugged"). Empty → the catalog's title.
        std::function<std::string()> dynamicTitle = {};
        /// False greys the menu row and disables the palette entry. Empty →
        /// always available.
        std::function<bool()> available = {};
        /// Draws the panel. Called only while it is visible.
        std::function<void()> draw = {};
        /// Called every frame, open or closed. For the one panel that has to
        /// see its own CLOSE: the //e keyboard latches Open-Apple / Solid-Apple,
        /// and a latch that outlives the window showing it as down is a key the
        /// guest holds forever with nothing left to release it. Such a panel
        /// keeps its own visibility guard and does its teardown on the edge.
        bool drawAlways = false;
        /// Offered nowhere in the UI — not greyed, absent. For a panel whose
        /// whole subsystem is compiled out on this platform (the AI control
        /// server under Emscripten), where a greyed row would advertise a
        /// feature that cannot exist rather than one that is unplugged.
        bool hidden = false;
    };

    /// Fresh-install visibility comes from the catalog, so "which panels does
    /// a new user see" is answered in the same table as everything else.
    PanelRegistry()
    {
        for (const PanelInfo& p : kPanelCatalog)
            visible_[index(p.id)] = p.defaultOpen;
    }

    // ── Visibility: the storage the 40 `bool showXxx` members used to be ──
    bool& visible(PanelId id) { return visible_[index(id)]; }
    bool  visible(PanelId id) const { return visible_[index(id)]; }

    // ── Runtime behaviour ────────────────────────────────────────────────
    /// Attach the live bits. Every field is optional; a panel that needs none
    /// of them (no card to check, a fixed title, drawn by the caller) simply
    /// never appears in the binding table.
    void bind(PanelId id, Runtime rt) { runtime_[index(id)] = std::move(rt); }
    void setDraw(PanelId id, std::function<void()> draw, bool always = false)
    {
        runtime_[index(id)].draw       = std::move(draw);
        runtime_[index(id)].drawAlways = always;
    }
    void hideFromUi(PanelId id) { runtime_[index(id)].hidden = true; }
    bool hidden(PanelId id) const { return runtime_[index(id)].hidden; }

    /// Label to draw: the dynamic one when there is one, else the catalog's.
    std::string title(PanelId id) const;
    /// Whether the panel can be opened at all right now.
    bool available(PanelId id) const;

    // ── The derived views ────────────────────────────────────────────────
    /// Every panel that carries a settings key, in catalog order.
    void forEachPersisted(
        const std::function<void(const char* key, bool& flag)>& fn);

    /// Draw every visible panel, in catalog order. Replaces the block of
    /// ~43 `if (showXxx) renderXxxWindow()` calls, whose order was the order
    /// somebody happened to add them in.
    void drawAll();

    /// Close every panel. Used by the browser build's chrome-light startup,
    /// which used to be 28 hand-written assignments and therefore missed
    /// every panel added after it was written.
    void hideAll();

    /// Panels with no `draw` — i.e. declared in the catalog and drawn by
    /// nobody. Checked at startup, because the symptom is a menu row that
    /// opens nothing.
    std::vector<std::string> undrawn() const;

private:
    static constexpr std::size_t index(PanelId id)
    {
        return static_cast<std::size_t>(id);
    }

    std::array<bool, kPanelCount>    visible_{};
    std::array<Runtime, kPanelCount> runtime_{};
};

}  // namespace pom2

#endif  // POM2_PANEL_REGISTRY_H

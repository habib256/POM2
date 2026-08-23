// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PanelRegistry — binds the static panel catalog to the running UI.
//
// `PanelCatalog.h` says what a panel IS (id, title, menu group, settings key,
// tooltip). This says what it is *right now*: which `bool` holds its
// visibility, whether the hardware behind it is plugged, and what its label
// reads when the label carries a slot number ("Printer (slot 1)").
//
// Everything the UI used to keep as a parallel list is derived from here:
//
//   for (const Binding& b : panels.all())            // the menus
//   panels.flag(id)                                  // the palette's dispatch
//   panels.forEachPersisted(...)                     // settings load + save
//   panels.hideAll()                                 // the WASM chrome-light
//
// ── Ownership ────────────────────────────────────────────────────────────
// The registry does NOT own the flags. It holds `bool*` into MainWindow's
// members, which keeps this change a rewiring rather than a rewrite of the
// ~150 sites that read those members directly. Moving the storage in here is
// the next step, and it is the one that finally deletes the wall of members.
//
// ── Threading ────────────────────────────────────────────────────────────
// UI thread only. A panel's visibility is not emulator state.

#ifndef POM2_PANEL_REGISTRY_H
#define POM2_PANEL_REGISTRY_H

#include "PanelCatalog.h"

#include <functional>
#include <string>
#include <vector>

namespace pom2 {

class PanelRegistry {
public:
    struct Binding {
        const PanelInfo* info    = nullptr;
        bool*            visible = nullptr;
        /// Runtime label, when the static title is not the whole story (a
        /// slot number, "no card plugged"). Empty → `info->title`.
        std::function<std::string()> dynamicTitle;
        /// False greys the row out and disables the palette entry. Empty →
        /// always available.
        std::function<bool()> available;
        /// Offered nowhere in the UI — not greyed, absent. For a panel whose
        /// whole subsystem is compiled out on this platform (the AI control
        /// server under Emscripten), where a greyed row would advertise a
        /// feature that cannot exist rather than one that is unplugged.
        bool hidden = false;
    };

    /// Bind a catalog id to its flag. An id that is not in the catalog is a
    /// programming error, reported through `unknownBinds()` rather than
    /// thrown: a mistyped id must not take the emulator down, and it must not
    /// pass unnoticed either.
    void bind(const char* id, bool* visible,
              std::function<bool()> available = {},
              std::function<std::string()> dynamicTitle = {});

    /// Take `id` out of the menus and the palette entirely. See Binding::hidden.
    void hideFromUi(const char* id);

    const std::vector<Binding>& all() const { return bindings_; }
    const Binding* find(const std::string& id) const;

    /// The visibility flag for `id`, or nullptr when unknown/unbound.
    bool* flag(const std::string& id) const;
    /// Flip `id`. False when there is no such panel — the caller can then
    /// fall through to its other command handling.
    bool toggle(const std::string& id) const;

    /// Label to draw: the dynamic one when there is one, else the catalog's.
    std::string title(const Binding& b) const;
    /// Whether the panel can be opened at all right now.
    bool available(const Binding& b) const;

    /// Every bound panel that carries a settings key, in catalog order.
    void forEachPersisted(const std::function<void(const char* key,
                                                   bool* flag)>& fn) const;

    /// Close every panel. Used by the browser build's chrome-light startup,
    /// which used to be 28 hand-written assignments and therefore missed
    /// every panel added after it was written.
    void hideAll() const;

    /// Catalog entries nobody bound — a panel declared and then forgotten,
    /// which would show up in the menus doing nothing. Checked at startup.
    std::vector<std::string> unbound() const;
    /// Ids passed to bind() that are not in the catalog (typos).
    const std::vector<std::string>& unknownBinds() const { return unknown_; }

private:
    std::vector<Binding>     bindings_;   ///< catalog order, by construction
    std::vector<std::string> unknown_;
};

}  // namespace pom2

#endif  // POM2_PANEL_REGISTRY_H

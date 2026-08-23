// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PanelRegistry — see PanelRegistry.h.

#include "PanelRegistry.h"

#include <algorithm>
#include <cstring>

namespace pom2 {
namespace {

const PanelInfo* catalogEntry(const char* id)
{
    if (!id) return nullptr;
    for (const PanelInfo& p : kPanelCatalog)
        if (std::strcmp(p.id, id) == 0) return &p;
    return nullptr;
}

/// Position of `info` in the catalog — the sort key that keeps the menus, the
/// palette and the settings file in the order the table is written in, so the
/// table stays the thing you read to know the UI.
std::size_t catalogIndex(const PanelInfo* info)
{
    for (std::size_t i = 0; i < kPanelCount; ++i)
        if (&kPanelCatalog[i] == info) return i;
    return kPanelCount;
}

}  // namespace

void PanelRegistry::bind(const char* id, bool* visible,
                         std::function<bool()> available,
                         std::function<std::string()> dynamicTitle)
{
    const PanelInfo* info = catalogEntry(id);
    if (!info || !visible) {
        unknown_.push_back(id ? id : "(null)");
        return;
    }
    // Re-binding the same panel replaces the old binding rather than adding a
    // second one: two rows for one window would be exactly the duplication
    // this class exists to remove.
    for (Binding& b : bindings_) {
        if (b.info == info) {
            b.visible      = visible;
            b.available    = std::move(available);
            b.dynamicTitle = std::move(dynamicTitle);
            return;
        }
    }
    Binding b;
    b.info         = info;
    b.visible      = visible;
    b.available    = std::move(available);
    b.dynamicTitle = std::move(dynamicTitle);
    bindings_.push_back(std::move(b));
    std::sort(bindings_.begin(), bindings_.end(),
              [](const Binding& x, const Binding& y) {
                  return catalogIndex(x.info) < catalogIndex(y.info);
              });
}

void PanelRegistry::hideFromUi(const char* id)
{
    const PanelInfo* info = catalogEntry(id);
    if (!info) { unknown_.push_back(id ? id : "(null)"); return; }
    for (Binding& b : bindings_)
        if (b.info == info) { b.hidden = true; return; }
}

const PanelRegistry::Binding* PanelRegistry::find(const std::string& id) const
{
    for (const Binding& b : bindings_)
        if (id == b.info->id) return &b;
    return nullptr;
}

bool* PanelRegistry::flag(const std::string& id) const
{
    const Binding* b = find(id);
    return b ? b->visible : nullptr;
}

bool PanelRegistry::toggle(const std::string& id) const
{
    bool* f = flag(id);
    if (!f) return false;
    *f = !*f;
    return true;
}

std::string PanelRegistry::title(const Binding& b) const
{
    if (b.dynamicTitle) return b.dynamicTitle();
    return b.info->title;
}

bool PanelRegistry::available(const Binding& b) const
{
    return b.available ? b.available() : true;
}

void PanelRegistry::forEachPersisted(
    const std::function<void(const char*, bool*)>& fn) const
{
    for (const Binding& b : bindings_)
        if (b.info->settingsKey && b.visible) fn(b.info->settingsKey, b.visible);
}

void PanelRegistry::hideAll() const
{
    for (const Binding& b : bindings_)
        if (b.visible) *b.visible = false;
}

std::vector<std::string> PanelRegistry::unbound() const
{
    std::vector<std::string> out;
    for (const PanelInfo& p : kPanelCatalog) {
        bool found = false;
        for (const Binding& b : bindings_)
            if (b.info == &p) { found = true; break; }
        if (!found) out.emplace_back(p.id);
    }
    return out;
}

}  // namespace pom2

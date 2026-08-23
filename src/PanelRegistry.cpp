// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PanelRegistry — see PanelRegistry.h.

#include "PanelRegistry.h"

namespace pom2 {

std::string PanelRegistry::title(PanelId id) const
{
    const Runtime& rt = runtime_[index(id)];
    if (rt.dynamicTitle) return rt.dynamicTitle();
    return panelInfo(id).title;
}

bool PanelRegistry::available(PanelId id) const
{
    const Runtime& rt = runtime_[index(id)];
    return rt.available ? rt.available() : true;
}

void PanelRegistry::forEachPersisted(
    const std::function<void(const char*, bool&)>& fn)
{
    // Catalog order, not enum order: the settings file reads like the menus.
    for (const PanelInfo& p : kPanelCatalog)
        if (p.settingsKey) fn(p.settingsKey, visible(p.id));
}

void PanelRegistry::drawAll()
{
    for (const PanelInfo& p : kPanelCatalog) {
        const Runtime& rt = runtime_[index(p.id)];
        if (!rt.draw) continue;
        if (!visible(p.id) && !rt.drawAlways) continue;
        rt.draw();
    }
}

void PanelRegistry::hideAll()
{
    visible_.fill(false);
}

std::vector<std::string> PanelRegistry::undrawn() const
{
    std::vector<std::string> out;
    for (const PanelInfo& p : kPanelCatalog)
        if (!runtime_[index(p.id)].draw) out.emplace_back(p.command);
    return out;
}

}  // namespace pom2

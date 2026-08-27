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

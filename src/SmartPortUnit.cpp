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

#include "SmartPortUnit.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"

namespace pom2 {

std::unique_ptr<SmartPortUnit> makeSmartPortUnit(std::string_view kindKey)
{
    if (kindKey == SmartPort35Unit::kKindKey)  return std::make_unique<SmartPort35Unit>();
    if (kindKey == SmartPortHdvUnit::kKindKey) return std::make_unique<SmartPortHdvUnit>();
    return nullptr;
}

} // namespace pom2

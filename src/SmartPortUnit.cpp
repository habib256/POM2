// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

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

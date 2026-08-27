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

#include "SlotConfigurationCoordinator.h"

#include "CffaCard.h"
#include "ClockCard.h"
#include "DiskIICard.h"
#include "EchoPlusCard.h"
#include "EchoPlusTMS5220Card.h"
#include "FujiNetCard.h"
#include "GrapplerCard.h"
#include "LeChatMauveCard.h"
#include "Logger.h"
#include "Mockingboard.h"
#include "MouseCard.h"
#include "MouseCardAppleWin.h"
#include "PhasorCard.h"
#include "PrinterCard.h"
#include "ProDOSHardDiskCard.h"
#include "Settings.h"
#include "SlotBus.h"
#include "SmartPortCard.h"
#include "SoftCardZ80.h"
#include "SuperSerialCard.h"
#include "UthernetCard.h"
#include "UthernetIICard.h"

#include <string_view>

namespace pom2 {

namespace {

constexpr std::array<std::string_view, 8> kDefaultCards{
    "",             // slot 0: Language Card is owned by Memory
    "grappler",     // slot 1: Grappler+ parallel printer
    "mouseaw",      // slot 2: AppleWin HLE mouse
    "",             // slot 3: IIe 80-column firmware is internal
    "mockingboard", // slot 4: Mockingboard A/C
    "smartport35",  // slot 5: SmartPort 3.5"
    "diskii",       // slot 6: Disk II
    "chatmauve",    // slot 7: Le Chat Mauve RGB
};

std::string liveCardKey(const SlotPeripheral& peripheral)
{
    if (dynamic_cast<const DiskIICard*>(&peripheral)) return "diskii";
    if (dynamic_cast<const ProDOSHardDiskCard*>(&peripheral)) return "hdv";
    if (dynamic_cast<const pom2::CffaCard*>(&peripheral)) return "cffa";
    if (dynamic_cast<const pom2::SmartPortCard*>(&peripheral))
        return "smartport35";
    if (dynamic_cast<const SuperSerialCard*>(&peripheral)) return "ssc";
    if (dynamic_cast<const PrinterCard*>(&peripheral)) return "printer";
    if (dynamic_cast<const GrapplerCard*>(&peripheral)) return "grappler";
    if (dynamic_cast<const ClockCard*>(&peripheral)) return "clock";
    if (dynamic_cast<const SoftCardZ80*>(&peripheral)) return "softcard";
    if (dynamic_cast<const LeChatMauveCard*>(&peripheral)) return "chatmauve";
    if (dynamic_cast<const MouseCard*>(&peripheral)) return "mouse";
    if (dynamic_cast<const MouseCardAppleWin*>(&peripheral)) return "mouseaw";
    if (const auto* card = dynamic_cast<const MockingboardCard*>(&peripheral)) {
        return card->getVariant() == MockingboardCard::Variant::SoundII
            ? "mockingboard_c" : "mockingboard";
    }
    if (dynamic_cast<const PhasorCard*>(&peripheral)) return "phasor";
    if (dynamic_cast<const EchoPlusCard*>(&peripheral)) return "echoplus";
    if (dynamic_cast<const EchoPlusTMS5220Card*>(&peripheral))
        return "echoplus_tms";
    if (dynamic_cast<const pom2::UthernetCard*>(&peripheral))
        return "uthernet";
    if (dynamic_cast<const pom2::UthernetIICard*>(&peripheral))
        return "uthernet2";
    if (dynamic_cast<const pom2::FujiNetCard*>(&peripheral)) return "fujinet";
    return {};
}

} // namespace

bool SlotConfigurationCoordinator::isMultiInstance(
    const std::string& cardKey) noexcept
{
    return cardKey == "diskii" || cardKey == "cffa" ||
           cardKey == "smartport35";
}

const SlotConfigurationCoordinator::CardMap&
SlotConfigurationCoordinator::resolve(const Settings& settings,
                                      SystemProfile profile)
{
    effectivePlan_.fill({});
    for (int slot = 1; slot <= 7; ++slot) {
        const std::string key = "slot_" + std::to_string(slot) + "_card";
        effectivePlan_[slot] = settings.getString(
            key, std::string(kDefaultCards[slot]));
    }

    // Compatibility with the old clock checkbox. An explicit per-slot value
    // always wins; this branch is intentionally inert with today's slot-4
    // default unless an older configuration explicitly expected a clock.
    if (!settings.getBool("clock_card_enable", true) &&
        settings.getString("slot_4_card", "__missing__") == "__missing__" &&
        effectivePlan_[4] == "clock") {
        effectivePlan_[4].clear();
    }

    const auto& cfg = profileConfig(profile);
    bool builtinRgb = false;
    for (int slot = 1; slot <= 7; ++slot) {
        if (cfg.builtInSlots[slot].has_value() &&
            cfg.builtInSlots[slot]->cardKey == "chatmauve") {
            builtinRgb = true;
        }
    }

    for (int slot = 1; slot <= 7; ++slot) {
        if (cfg.builtInSlots[slot].has_value()) {
            const std::string& forced = cfg.builtInSlots[slot]->cardKey;
            if (effectivePlan_[slot] != forced) {
                log().info("Slots",
                    "Slot " + std::to_string(slot) + " forced to '" +
                    forced + "' (built-in on " +
                    std::string(cfg.displayName) + "); user setting '" +
                    effectivePlan_[slot] + "' ignored");
                effectivePlan_[slot] = forced;
            }
            continue;
        }

        if (!cfg.noPhysicalSlots || effectivePlan_[slot].empty()) continue;
        if (effectivePlan_[slot] == "chatmauve" && !builtinRgb) {
            log().info("Slots",
                "Slot " + std::to_string(slot) +
                " = Le Chat Mauve RGB (rear video-connector adapter) on " +
                std::string(cfg.displayName));
            continue;
        }

        log().info("Slots",
            "Slot " + std::to_string(slot) + " left empty on " +
            std::string(cfg.displayName) +
            " (no physical slot connector on this model); user setting '" +
            effectivePlan_[slot] + "' ignored");
        effectivePlan_[slot].clear();
    }

    // Profile fixtures are trusted and can legitimately repeat (the //c has
    // two built-in serial ports). User-selected cards are single-instance
    // unless their storage and settings models are explicitly per-slot.
    for (int slot = 1; slot <= 7; ++slot) {
        const std::string card = effectivePlan_[slot];
        if (card.empty() || isMultiInstance(card) ||
            cfg.builtInSlots[slot].has_value()) {
            continue;
        }

        int first = -1;
        for (int candidate = 1; candidate <= 7; ++candidate) {
            if (effectivePlan_[candidate] == card) {
                first = candidate;
                break;
            }
        }
        if (first == slot) continue;

        log().warn("Slots",
            "Slot " + std::to_string(slot) + " requested '" + card +
            "' but that card is already in slot " + std::to_string(first) +
            " — leaving slot " + std::to_string(slot) + " empty");
        effectivePlan_[slot].clear();
    }

    resetDraft();
    return effectivePlan_;
}

SlotConfigurationCoordinator::LiveSnapshot
SlotConfigurationCoordinator::captureLive(const SlotBus& bus) const
{
    LiveSnapshot snapshot;
    for (int slot = 1; slot < SlotBus::kSlotCount; ++slot) {
        const auto* peripheral = bus.peripheral(slot);
        if (!peripheral) continue;
        snapshot.keys[slot] = liveCardKey(*peripheral);
        snapshot.names[slot] = std::string(peripheral->name());
    }
    return snapshot;
}

} // namespace pom2

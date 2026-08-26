// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Central slot-card construction. This factory owns implementation selection,
// ROM discovery/validation and explicit fallbacks. Runtime wiring (CPU, audio,
// transports, mounted media and SlotBus ownership) remains with the composer
// or the additive session provisioner.

#ifndef POM2_SLOT_CARD_FACTORY_H
#define POM2_SLOT_CARD_FACTORY_H

#include "SlotPeripheral.h"
#include "SystemProfile.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace pom2 {

class SlotCardFactory final
{
public:
    using ResourceLocator =
        std::function<std::string(std::string_view resource)>;

    struct Request {
        std::string key;
        int slot = -1;
        bool cpuIsCmos = false;
        SystemProfile profile = SystemProfile::AppleIIe;
    };

    struct Result {
        std::unique_ptr<SlotPeripheral> card;
        std::string requestedKey;
        std::string actualKey;
        std::string resourcePath;
        std::string status;
        std::string warningCategory;
        std::string warning;
        bool fallback = false;

        explicit operator bool() const noexcept { return card != nullptr; }
    };

    /// An empty locator selects the production ResourcePaths adapter.
    explicit SlotCardFactory(ResourceLocator locator = {});

    /// Handles the factory-owned subset of the slot catalog. Unknown or
    /// deliberately composer-owned keys return an empty Result with no warning.
    Result create(const Request& request) const;

private:
    Result createMouse(const Request& request) const;
    ResourceLocator locate_;
};

} // namespace pom2

#endif // POM2_SLOT_CARD_FACTORY_H

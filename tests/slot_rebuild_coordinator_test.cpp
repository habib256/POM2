// Transactional SlotBus rebuild order and phase contract.

#include "EmulationController.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SlotRebuildCoordinator.h"

#include <cassert>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

class ProbeCard final : public SlotPeripheral
{
public:
    explicit ProbeCard(std::vector<std::string>& events) : events_(events) {}
    ~ProbeCard() override { events_.push_back("clear-slot-bus"); }
    std::string_view name() const override { return "rebuild probe"; }

private:
    std::vector<std::string>& events_;
};

void expectLogicError(const std::function<void()>& operation)
{
    bool rejected = false;
    try {
        operation();
    } catch (const std::logic_error&) {
        rejected = true;
    }
    assert(rejected);
}

} // namespace

int main()
{
    EmulationController controller;
    std::vector<std::string> events;

    // No lifecycle edge is optional: a partially-wired coordinator would
    // make the apparent transaction less safe than the explicit old code.
    expectLogicError([] {
        pom2::SlotRebuildCoordinator invalid({});
    });

    pom2::SlotRebuildCoordinator rebuild({
        [&] { events.push_back("invalidate-history"); },
        [&] { events.push_back("detach-control"); },
        [&] { events.push_back("detach-audio"); },
        [&] { events.push_back("detach-frontend"); },
        [&] { events.push_back("reset-printer"); },
        [&] { events.push_back("stop-network"); },
        [&] { events.push_back("detach-display"); },
        [&] { events.push_back("publish-control"); },
    });

    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::Stable);
    assert(rebuild.generation() == 0);

    // The bus cannot be cleared without first proving media was flushed.
    {
        auto state = controller.lockState();
        expectLogicError([&] { rebuild.beginLocked(state); });
    }
    assert(events.empty());

    rebuild.prepareAfterFlush();
    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::Prepared);
    assert(rebuild.generation() == 1);
    assert((events == std::vector<std::string>{"invalidate-history"}));
    expectLogicError([&] { rebuild.prepareAfterFlush(); });

    {
        auto state = controller.lockState();
        state.memory().slotBus().plug(4, std::make_unique<ProbeCard>(events));
        rebuild.beginLocked(state);
        assert(!state.memory().slotBus().isPlugged(4));
        assert(rebuild.phase() ==
               pom2::SlotRebuildCoordinator::Phase::Rebuilding);
        expectLogicError([&] { rebuild.beginLocked(state); });

        const std::vector<std::string> expected{
            "invalidate-history",
            "detach-control",
            "detach-audio",
            "detach-frontend",
            "reset-printer",
            "clear-slot-bus",
            "stop-network",
            "detach-display",
        };
        assert(events == expected);

        // Publication occurs only once the caller declares the rebuilt
        // topology coherent while still holding stateMutex.
        rebuild.publishLocked(state);
    }

    assert(rebuild.phase() ==
           pom2::SlotRebuildCoordinator::Phase::Stable);
    assert(events.back() == "publish-control");

    {
        auto state = controller.lockState();
        expectLogicError([&] { rebuild.publishLocked(state); });
    }
    assert(events.back() == "publish-control");

    return 0;
}

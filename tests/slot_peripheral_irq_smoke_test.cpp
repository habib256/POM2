// SlotPeripheral IRQ-line smoke test. Pins the contract added when we
// exposed `assertIrq()` on SlotPeripheral:
//
//   1. A card plugged into a SlotBus with an installed IrqRouter
//      forwards `assertIrq(true/false)` to the router with the slot
//      number the bus assigned at plug-time. Cards no longer need to
//      hold an M6502* or know which slot they live in.
//   2. `assertIrq()` is idempotent: repeated true→true / false→false
//      are no-ops on the router (debounced by the base class).
//   3. `SlotBus::plug()` and `SlotBus::unplug()` auto-release any
//      pending IRQ contribution before letting the card go, so a
//      profile switch or hot-swap never leaves a "stuck" wire-OR bit
//      stuck on the aggregator side. This was the bug Mockingboard /
//      SSC / Mouse `onUnplug` handlers used to fix manually — the
//      auto-release moves that into the base class.
//   4. With no router installed, `assertIrq()` is a no-op (cards
//      still update their `slotIrqAsserted()` cache so test
//      introspection works in headless harnesses).
//   5. Before plug, `assertIrq()` is silent — the bus pointer is
//      null so a stray assertion during construction can't crash.
//
// No Memory / M6502 dependency: we install a recording closure as the
// router so the test pins the SlotBus → router contract in isolation.

#include "SlotBus.h"
#include "SlotPeripheral.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// Tiny card that exposes assertIrq publicly. Mirrors what real cards
// (Mockingboard / SSC / Mouse) do internally: just forward a bool.
class StubCard : public SlotPeripheral
{
public:
    std::string_view name() const override { return "stub"; }
    using SlotPeripheral::assertIrq;
};

struct IrqEvent { int slot; bool asserted; };

}  // namespace

int main()
{
    // ─── Test 1: plug + assertIrq routes through the installed sink ─────
    {
        SlotBus bus;
        std::vector<IrqEvent> events;
        bus.setIrqRouter([&events](int slot, bool a) {
            events.push_back({ slot, a });
        });

        auto stub = std::make_unique<StubCard>();
        StubCard* ptr = stub.get();
        bus.plug(4, std::move(stub));

        assert(ptr->busSlot() == 4);
        assert(!ptr->slotIrqAsserted());

        ptr->assertIrq(true);
        assert(events.size() == 1);
        assert(events[0].slot == 4);
        assert(events[0].asserted == true);
        assert(ptr->slotIrqAsserted());

        ptr->assertIrq(false);
        assert(events.size() == 2);
        assert(events[1].slot == 4);
        assert(events[1].asserted == false);
        assert(!ptr->slotIrqAsserted());

        std::printf("[ OK ] assertIrq routes via the SlotBus sink\n");
    }

    // ─── Test 2: idempotent assert/release (no extra edges) ─────────────
    {
        SlotBus bus;
        std::vector<IrqEvent> events;
        bus.setIrqRouter([&events](int slot, bool a) {
            events.push_back({ slot, a });
        });
        auto stub = std::make_unique<StubCard>();
        StubCard* ptr = stub.get();
        bus.plug(2, std::move(stub));

        ptr->assertIrq(true);
        ptr->assertIrq(true);   // dup
        ptr->assertIrq(true);   // dup
        assert(events.size() == 1);

        ptr->assertIrq(false);
        ptr->assertIrq(false);  // dup
        assert(events.size() == 2);

        std::printf("[ OK ] idempotent assert/release debounces\n");
    }

    // ─── Test 3: unplug auto-releases a pending IRQ ─────────────────────
    {
        SlotBus bus;
        std::vector<IrqEvent> events;
        bus.setIrqRouter([&events](int slot, bool a) {
            events.push_back({ slot, a });
        });

        auto stub = std::make_unique<StubCard>();
        StubCard* ptr = stub.get();
        bus.plug(6, std::move(stub));

        ptr->assertIrq(true);
        assert(events.size() == 1);
        assert(events.back().slot == 6 && events.back().asserted == true);

        // Unplug returns the card and releases the wire-OR bit. This is
        // the bug fix for "stuck IRQ across profile switch" — cards no
        // longer have to remember to clear in their own onUnplug.
        auto out = bus.unplug(6);
        assert(out.get() == ptr);
        assert(events.size() == 2);
        assert(events.back().slot == 6 && events.back().asserted == false);

        // Right after unplug the cache is reset and the bus pointer is
        // null — the card looks "idle" again so it's safe to re-plug.
        assert(!ptr->slotIrqAsserted());
        assert(ptr->busSlot() == -1);

        // Stray assertions after unplug update the local cache (so the
        // card stays observable in headless tests) but DO NOT reach the
        // router — the wire-OR aggregator must not learn about a card
        // that no longer exists from its perspective.
        ptr->assertIrq(true);
        assert(events.size() == 2);
        assert(ptr->slotIrqAsserted());     // cache-only update
        ptr->assertIrq(false);              // re-zero before drop
        assert(events.size() == 2);

        std::printf("[ OK ] unplug auto-releases the IRQ bit\n");
    }

    // ─── Test 4: replacing a card in the same slot releases the old IRQ ─
    {
        SlotBus bus;
        std::vector<IrqEvent> events;
        bus.setIrqRouter([&events](int slot, bool a) {
            events.push_back({ slot, a });
        });

        auto first = std::make_unique<StubCard>();
        StubCard* firstPtr = first.get();
        bus.plug(3, std::move(first));
        firstPtr->assertIrq(true);
        assert(events.size() == 1);

        // Plug a different card into the same slot — the previous card
        // is destroyed; its still-asserted bit must release before it
        // goes, or the wire-OR aggregator sees a phantom IRQ pulled by
        // a dead card.
        bus.plug(3, std::make_unique<StubCard>());
        assert(events.size() == 2);
        assert(events.back().slot == 3 && events.back().asserted == false);

        std::printf("[ OK ] re-plug clears the outgoing card's IRQ\n");
    }

    // ─── Test 5: clear() releases every plugged card's IRQ ──────────────
    {
        SlotBus bus;
        std::vector<IrqEvent> events;
        bus.setIrqRouter([&events](int slot, bool a) {
            events.push_back({ slot, a });
        });

        auto a = std::make_unique<StubCard>();
        auto b = std::make_unique<StubCard>();
        StubCard* aPtr = a.get();
        StubCard* bPtr = b.get();
        bus.plug(1, std::move(a));
        bus.plug(7, std::move(b));
        aPtr->assertIrq(true);
        bPtr->assertIrq(true);
        assert(events.size() == 2);

        // Profile switch tears the whole bus down in one shot.
        events.clear();
        bus.clear();
        // Both cards must release before destruction. Order isn't
        // contractual — the test only pins that both edges fire.
        bool seen1 = false, seen7 = false;
        for (const auto& ev : events) {
            assert(ev.asserted == false);
            if (ev.slot == 1) seen1 = true;
            if (ev.slot == 7) seen7 = true;
        }
        assert(seen1 && seen7);

        std::printf("[ OK ] clear() releases every card's IRQ bit\n");
    }

    // ─── Test 6: no router → assertIrq updates cache only, never fires ──
    {
        SlotBus bus;            // router intentionally not installed
        auto stub = std::make_unique<StubCard>();
        StubCard* ptr = stub.get();
        bus.plug(5, std::move(stub));

        ptr->assertIrq(true);
        assert(ptr->slotIrqAsserted());     // local cache updated
        ptr->assertIrq(false);
        assert(!ptr->slotIrqAsserted());

        std::printf("[ OK ] no router → cache-only assert (no crash)\n");
    }

    // ─── Test 7: assertIrq before plug is a silent no-op ────────────────
    {
        StubCard standalone;
        assert(standalone.busSlot() == -1);
        standalone.assertIrq(true);
        // Cache still tracks the request locally so tests don't have to
        // know whether the card is plugged.
        assert(standalone.slotIrqAsserted());
        standalone.assertIrq(false);
        assert(!standalone.slotIrqAsserted());

        std::printf("[ OK ] pre-plug assertIrq is silent\n");
    }

    std::printf("All SlotPeripheral IRQ smoke tests passed.\n");
    return 0;
}

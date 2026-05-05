// SlotBus dispatch smoke test — pins:
//   * device-select decode (slot N at $C080+N*16, low4 = addr & 0xF)
//   * slot-ROM decode (slot N at $CN00, low8 = addr & 0xFF)
//   * active-expansion-slot latching by any $CnXX access (reads only —
//     writes-into-rom strobes go through the same path in production)
//   * $CFFF deactivates the expansion ROM (read or write)
//   * empty slot returns open-bus $FF on slot ROM and on expansion ROM
//   * unplug() of the active slot clears the latch

#include "SlotBus.h"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {

class FakeCard : public SlotPeripheral
{
public:
    explicit FakeCard(uint8_t signature) : sig(signature) {}
    std::string_view name() const override { return "FakeCard"; }

    uint8_t deviceSelectRead(uint8_t low4) override {
        ++deviceReads;
        return static_cast<uint8_t>(low4 | sig);
    }
    void deviceSelectWrite(uint8_t low4, uint8_t v) override {
        ++deviceWrites;
        lastWriteLow4 = low4;
        lastWriteValue = v;
    }
    uint8_t slotRomRead(uint8_t low8) override {
        ++slotRomReads;
        return static_cast<uint8_t>(low8 ^ sig);
    }
    uint8_t expansionRomRead(uint16_t offset) override {
        ++expansionReads;
        return static_cast<uint8_t>(offset & 0xFF);
    }
    void expansionRomWrite(uint16_t offset, uint8_t v) override {
        ++expansionWrites;
        lastExpOffset = offset;
        lastExpValue  = v;
    }
    void onPlug()   override { ++plugCount; }
    void onUnplug() override { ++unplugCount; }
    void onReset()  override { ++resetCount; }

    int deviceReads     = 0;
    int deviceWrites    = 0;
    int slotRomReads    = 0;
    int expansionReads  = 0;
    int expansionWrites = 0;
    int plugCount       = 0;
    int unplugCount     = 0;
    int resetCount      = 0;
    uint8_t  sig;
    uint8_t  lastWriteLow4   = 0;
    uint8_t  lastWriteValue  = 0;
    uint16_t lastExpOffset   = 0;
    uint8_t  lastExpValue    = 0;
};

} // namespace

int main()
{
    SlotBus bus;

    // Plug card in slot 6 (typical Disk II location).
    auto cardPtr = std::make_unique<FakeCard>(0xA0);
    FakeCard* card = cardPtr.get();
    bus.plug(6, std::move(cardPtr));
    assert(bus.isPlugged(6));
    assert(card->plugCount == 1);

    // Device select: slot 6 = $C0E0-$C0EF.
    assert(bus.deviceSelectRead(0xC0E0) == (0x00 | 0xA0));
    assert(bus.deviceSelectRead(0xC0EF) == (0x0F | 0xA0));
    bus.deviceSelectWrite(0xC0E5, 0x42);
    assert(card->lastWriteLow4 == 0x05);
    assert(card->lastWriteValue == 0x42);

    // Slot ROM read: slot 6 ROM at $C600-$C6FF.
    const uint8_t got = bus.slotRomRead(0xC600);
    assert(got == (0x00 ^ 0xA0));
    // After any $C6xx access, slot 6 is the active expansion ROM owner.
    assert(bus.getActiveExpansionSlot() == 6);

    // Empty slot ROM reads should return open-bus $FF AND still latch the
    // expansion-active slot (matches hardware decode).
    assert(bus.slotRomRead(0xC400) == 0xFF);
    assert(bus.getActiveExpansionSlot() == 4);

    // Re-select slot 6 then read expansion ROM.
    (void)bus.slotRomRead(0xC601);
    assert(bus.getActiveExpansionSlot() == 6);
    const uint8_t expByte = bus.expansionRomRead(0xC842);
    assert(expByte == 0x42);  // (0xC842 - 0xC800) & 0xFF == 0x42
    assert(card->expansionReads == 1);

    // $CFFF disables expansion ROM.
    (void)bus.expansionRomRead(0xCFFF);
    assert(bus.getActiveExpansionSlot() == -1);
    // Read after disable should NOT route to the card.
    const int prevExpansionReads = card->expansionReads;
    assert(bus.expansionRomRead(0xC900) == 0xFF);
    assert(card->expansionReads == prevExpansionReads);

    // Re-arm and write to expansion ROM.
    (void)bus.slotRomRead(0xC600);
    bus.expansionRomWrite(0xC820, 0x99);
    assert(card->lastExpOffset == 0x0020);
    assert(card->lastExpValue  == 0x99);

    // Reset hits all plugged cards.
    bus.reset();
    assert(card->resetCount == 1);

    // Unplug clears latch when active slot is removed.
    auto removed = bus.unplug(6);
    assert(removed != nullptr);
    assert(card->unplugCount == 1);
    assert(!bus.isPlugged(6));
    assert(bus.getActiveExpansionSlot() == -1);

    std::printf("SlotBus smoke: OK\n");
    return 0;
}

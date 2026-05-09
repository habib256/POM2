// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SlotBus.h"

void SlotBus::plug(int slot, std::unique_ptr<SlotPeripheral> card)
{
    if (slot < 0 || slot >= kSlotCount) return;

    if (slots[slot]) {
        slots[slot]->onUnplug();
        if (activeExpansionSlot == slot) activeExpansionSlot = -1;
        slots[slot].reset();
    }
    if (card) {
        slots[slot] = std::move(card);
        slots[slot]->onPlug();
    }
}

std::unique_ptr<SlotPeripheral> SlotBus::unplug(int slot)
{
    if (slot < 0 || slot >= kSlotCount) return nullptr;
    if (!slots[slot]) return nullptr;
    slots[slot]->onUnplug();
    if (activeExpansionSlot == slot) activeExpansionSlot = -1;
    return std::move(slots[slot]);
}

uint8_t SlotBus::deviceSelectRead(uint16_t addr)
{
    // $C080-$C0FF — 16 bytes per slot. Slot N starts at $C080 + N*16.
    if (addr < 0xC080 || addr > 0xC0FF) return 0xFF;
    const int slot = (addr - 0xC080) >> 4;
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);
    if (auto* p = slots[slot].get()) return p->deviceSelectRead(low4);
    return 0;     // empty slot — Apple II floats the bus low here
}

void SlotBus::deviceSelectWrite(uint16_t addr, uint8_t v)
{
    if (addr < 0xC080 || addr > 0xC0FF) return;
    const int slot = (addr - 0xC080) >> 4;
    const uint8_t low4 = static_cast<uint8_t>(addr & 0x0F);
    if (auto* p = slots[slot].get()) p->deviceSelectWrite(low4, v);
}

uint8_t SlotBus::slotRomRead(uint16_t addr)
{
    // $C100-$C7FF — slot N at $C(N)00-$C(N)FF, N=1..7.
    if (addr < 0xC100 || addr > 0xC7FF) return 0xFF;
    const int slot = (addr >> 8) & 0x07;     // $CN00 → N
    if (slot < 1 || slot > 7) return 0xFF;

    // Critical Apple II behaviour: any access to a slot's ROM window
    // marks that slot as the active expansion-ROM owner, even when the
    // slot is empty. Cards rely on this to load the right code into the
    // shared $C800-$CFFF window before jumping into it.
    activeExpansionSlot = slot;

    if (auto* p = slots[slot].get())
        return p->slotRomRead(static_cast<uint8_t>(addr & 0xFF));
    return 0xFF;     // open bus on empty slot
}

uint8_t SlotBus::expansionRomRead(uint16_t addr)
{
    if (addr < 0xC800 || addr > 0xCFFF) return 0xFF;
    if (addr == 0xCFFF) {
        // Read or write to $CFFF deactivates the expansion ROM. The byte
        // returned is the open-bus floater — Apple II hardware leaves the
        // last fetched byte on the bus, but $FF is a safe approximation.
        deactivateExpansion();
        return 0xFF;
    }
    const int slot = activeExpansionSlot;
    if (slot < 1 || slot > 7) return 0xFF;
    if (auto* p = slots[slot].get())
        return p->expansionRomRead(static_cast<uint16_t>(addr - 0xC800));
    return 0xFF;
}

void SlotBus::expansionRomWrite(uint16_t addr, uint8_t v)
{
    if (addr < 0xC800 || addr > 0xCFFF) return;
    if (addr == 0xCFFF) { deactivateExpansion(); return; }
    const int slot = activeExpansionSlot;
    if (slot < 1 || slot > 7) return;
    if (auto* p = slots[slot].get())
        p->expansionRomWrite(static_cast<uint16_t>(addr - 0xC800), v);
}

void SlotBus::advanceCycles(int cycles)
{
    if (cycles <= 0) return;
    for (auto& s : slots) if (s) s->advanceCycles(cycles);
}

void SlotBus::broadcastVideoSwitch(uint16_t addr)
{
    for (auto& s : slots) if (s) s->onVideoSoftSwitch(addr);
}

void SlotBus::reset()
{
    for (auto& s : slots) if (s) s->onReset();
    // Note: we deliberately do NOT clear activeExpansionSlot — Apple II
    // hardware reset doesn't drop the expansion-ROM enable, the latch is
    // cleared only by $CFFF or by losing the slot's $CnXX access.
}

void SlotBus::clear()
{
    for (auto& s : slots) {
        if (s) {
            s->onUnplug();
            s.reset();
        }
    }
    activeExpansionSlot = -1;
}

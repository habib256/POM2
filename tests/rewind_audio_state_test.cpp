// Rewind audio-chip state test (Phase 4, audio).
//
// Mockingboard / Phasor sound chips (6522 VIAs + AY-3-8910s) now serialize
// their register/timer state via SlotPeripheral::append/loadSnapshotState, so
// a rewind during music restores the chips instead of leaving them droning on
// stale registers. This pins:
//   1. a driven Mockingboard's chip state is captured (driven blob ≠ fresh)
//      and round-trips through a full-machine snapshot bit-for-bit;
//   2. restoring into a machine with no card in that slot is a safe no-op;
//   3. a foreign/short blob is ignored (magic + version guard).

#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "Mockingboard.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

constexpr int kSlot = 4;

// AY control-bus commands on VIA Port B (RESET=PB2 held high = not in reset).
constexpr uint8_t kPbReset    = 0x04;          // RESET only → inactive
constexpr uint8_t kPbLatch    = 0x04 | 0x03;   // RESET+BDIR+BC1 → latch addr
constexpr uint8_t kPbWrite    = 0x04 | 0x02;   // RESET+BDIR     → write
constexpr uint8_t kPbInactive = kPbReset;

void writeVia(SlotPeripheral* c, int chip, uint8_t reg, uint8_t v)
{
    const uint8_t base = (chip == 0) ? 0x00 : 0x80;
    c->slotRomWrite(static_cast<uint8_t>(base | (reg & 0x0F)), v);
}

// Store `v` into AY register `reg` on `chip` via the VIA strobe sequence a
// real music driver uses.
void ayWrite(SlotPeripheral* c, int chip, uint8_t reg, uint8_t v)
{
    writeVia(c, chip, 0x03, 0xFF);          // DDRA = all output
    writeVia(c, chip, 0x02, 0x07);          // DDRB = PB0..2 output
    writeVia(c, chip, 0x01, reg);           // ORA = reg addr
    writeVia(c, chip, 0x00, kPbLatch);      // latch
    writeVia(c, chip, 0x00, kPbInactive);
    writeVia(c, chip, 0x01, v);             // ORA = data
    writeVia(c, chip, 0x00, kPbWrite);      // write
    writeVia(c, chip, 0x00, kPbInactive);
}

void driveCard(SlotPeripheral* c)
{
    // A little tune-like register spray across both AYs + some VIA timer state.
    for (int chip = 0; chip < 2; ++chip) {
        ayWrite(c, chip, 0,  0x55);   // tone A period low
        ayWrite(c, chip, 1,  0x01);   // tone A period high
        ayWrite(c, chip, 7,  0x38);   // mixer: tones on
        ayWrite(c, chip, 8,  0x0F);   // channel A volume
        ayWrite(c, chip, 11, 0x20);   // envelope period low
        ayWrite(c, chip, 13, 0x0A);   // envelope shape
    }
    // Arm a VIA timer so t1Counter/ifr differ from the default.
    writeVia(c, 0, 0x0B, 0x40);       // ACR: T1 continuous
    writeVia(c, 0, 0x04, 0x00);       // T1CL
    writeVia(c, 0, 0x05, 0x10);       // T1CH → loads + starts T1
    c->advanceCycles(5000);
}

std::vector<uint8_t> fullCapture(M6502& cpu, Memory& mem)
{
    std::vector<uint8_t> blob;
    pom2::SnapshotWriter w(blob);
    pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
    return blob;
}

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    mem.slotBus().plug(kSlot, std::make_unique<MockingboardCard>(kSlot));
    SlotPeripheral* card = mem.slotBus().peripheral(kSlot);
    assert(card);
    driveCard(card);

    // (1) Chip state captured (non-trivial) and round-trips exactly.
    std::vector<uint8_t> drivenBlob;
    card->appendSnapshotState(drivenBlob);
    assert(!drivenBlob.empty());
    {
        MockingboardCard fresh(kSlot);
        std::vector<uint8_t> freshBlob;
        fresh.appendSnapshotState(freshBlob);
        assert(drivenBlob != freshBlob && "driveCard changed no captured chip state");
    }

    const std::vector<uint8_t> full1 = fullCapture(cpu, mem);
    {
        Memory mem2;
        M6502  cpu2(&mem2);
        mem2.slotBus().plug(kSlot, std::make_unique<MockingboardCard>(kSlot));

        pom2::SnapshotReader r(full1.data(), full1.size());
        assert(r.good());
        assert(pom2::restoreMachineState(r, cpu2, mem2).ok);

        std::vector<uint8_t> restoredCardBlob;
        mem2.slotBus().peripheral(kSlot)->appendSnapshotState(restoredCardBlob);
        assert(restoredCardBlob == drivenBlob && "AY/VIA state did not round-trip");

        const std::vector<uint8_t> full2 = fullCapture(cpu2, mem2);
        assert(full2 == full1 && "full machine (incl. Mockingboard) did not round-trip");
    }

    // (2) Restoring into a slot with no card is a safe no-op.
    {
        Memory mem3;
        M6502  cpu3(&mem3);
        pom2::SnapshotReader r(full1.data(), full1.size());
        assert(r.good());
        assert(pom2::restoreMachineState(r, cpu3, mem3).ok);
    }

    // (2b) SSI263 speech state round-trips (Sound II variant). Exercises the
    //      present&0x10 SSI path in the card snapshot.
    {
        Memory mem2;
        M6502  cpu2(&mem2);
        mem2.slotBus().plug(kSlot,
            std::make_unique<MockingboardCard>(kSlot, MockingboardCard::Variant::SoundII));
        SlotPeripheral* sc = mem2.slotBus().peripheral(kSlot);
        // Write SSI registers at $Cs40-$Cs44 → non-default chip + a phoneme.
        sc->slotRomWrite(0x43, 0x70);   // CTTRAMP (control / amplitude)
        sc->slotRomWrite(0x42, 0x40);   // RATEINF
        sc->slotRomWrite(0x40, 0x05);   // DURPHON → triggers a phoneme
        sc->advanceCycles(3000);

        std::vector<uint8_t> driven;
        sc->appendSnapshotState(driven);
        MockingboardCard freshSoundII(kSlot, MockingboardCard::Variant::SoundII);
        std::vector<uint8_t> fresh;
        freshSoundII.appendSnapshotState(fresh);
        assert(driven != fresh && "SSI263 write changed no captured state");

        const std::vector<uint8_t> full = fullCapture(cpu2, mem2);
        Memory mem3;
        M6502  cpu3(&mem3);
        mem3.slotBus().plug(kSlot,
            std::make_unique<MockingboardCard>(kSlot, MockingboardCard::Variant::SoundII));
        pom2::SnapshotReader r(full.data(), full.size());
        assert(r.good());
        assert(pom2::restoreMachineState(r, cpu3, mem3).ok);
        std::vector<uint8_t> restored;
        mem3.slotBus().peripheral(kSlot)->appendSnapshotState(restored);
        assert(restored == driven && "SSI263 + AY/VIA state did not round-trip");
    }

    // (3) A foreign / truncated blob is ignored.
    {
        MockingboardCard cardC(kSlot);
        std::vector<uint8_t> before;
        cardC.appendSnapshotState(before);
        const uint8_t junk[5] = { 'Z', 'Z', 'Z', 0x09, 0x09 };
        cardC.loadSnapshotState(junk, sizeof(junk));
        std::vector<uint8_t> after;
        cardC.appendSnapshotState(after);
        assert(after == before && "card mutated on a foreign blob");
    }

    std::printf("Rewind audio state: OK (Mockingboard AY/VIA round-trip + guards)\n");
    return 0;
}

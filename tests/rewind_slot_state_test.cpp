// Rewind slot-state test (Phase 4).
//
// Pins that per-slot card state rides along in a machine snapshot, so a
// rewind during disk I/O restores the drive head/LSS instead of leaving an
// in-progress read on the wrong nibble. Covers:
//   1. DiskIICard::appendSnapshotState captures mutated state (the driven
//      blob differs from a fresh card) and round-trips through
//      loadSnapshotState bit-for-bit;
//   2. MachineSnapshot writes/reads the "SLOT6" section: a full-machine
//      capture → restore (into a second machine with a fresh card) →
//      re-capture is byte-identical;
//   3. restoring into a machine with NO card in that slot is a safe no-op;
//   4. a card ignores a foreign / truncated blob (magic + version guard).

#include "DiskIICard.h"
#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

// Drive a Disk II controller into a clearly non-default mechanical/LSS state:
// motor on, read mode, and step the head across several quarter-tracks.
void driveCard(SlotPeripheral* c)
{
    c->deviceSelectRead(0x9);   // motor on
    c->deviceSelectRead(0xA);   // select drive 1
    c->deviceSelectRead(0xE);   // Q7L — read mode
    c->deviceSelectRead(0xC);   // Q6L — shift/read
    for (int i = 0; i < 10; ++i) {
        const int phase = i % 4;
        c->deviceSelectRead(static_cast<uint8_t>(phase * 2 + 1));  // phase ON
        c->advanceCycles(1500);
        c->deviceSelectRead(static_cast<uint8_t>(phase * 2));      // phase OFF
        c->advanceCycles(1500);
        c->deviceSelectRead(0xC);                                  // read a nibble
    }
    c->advanceCycles(8000);
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
    // ── Source machine with a driven Disk II in slot 6 ────────────────────
    Memory mem;
    M6502  cpu(&mem);
    mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));
    SlotPeripheral* card = mem.slotBus().peripheral(6);
    assert(card);
    driveCard(card);

    // (1) Card serialization is non-trivial and round-trips exactly.
    std::vector<uint8_t> drivenBlob;
    card->appendSnapshotState(drivenBlob);
    assert(!drivenBlob.empty());
    {
        DiskIICard fresh(6);
        std::vector<uint8_t> freshBlob;
        fresh.appendSnapshotState(freshBlob);
        assert(drivenBlob != freshBlob && "driveCard did not change any captured state");
    }
    {
        DiskIICard cardB(6);
        cardB.loadSnapshotState(drivenBlob.data(), drivenBlob.size());
        std::vector<uint8_t> blobB;
        cardB.appendSnapshotState(blobB);
        assert(blobB == drivenBlob && "card state did not round-trip");
    }

    // (2) Full-machine snapshot carries SLOT6 and restores bit-exact.
    const std::vector<uint8_t> full1 = fullCapture(cpu, mem);
    {
        Memory mem2;
        M6502  cpu2(&mem2);
        mem2.slotBus().plug(6, std::make_unique<DiskIICard>(6));

        pom2::SnapshotReader r(full1.data(), full1.size());
        assert(r.good());
        const auto res = pom2::restoreMachineState(r, cpu2, mem2);
        assert(res.ok);

        // The restored card must match the source card …
        std::vector<uint8_t> restoredCardBlob;
        mem2.slotBus().peripheral(6)->appendSnapshotState(restoredCardBlob);
        assert(restoredCardBlob == drivenBlob);

        // … and the whole machine re-serializes identically.
        const std::vector<uint8_t> full2 = fullCapture(cpu2, mem2);
        assert(full2 == full1 && "full machine (incl. SLOT6) did not round-trip");
    }

    // (3) Restoring into a machine with no card in slot 6 is a safe no-op.
    {
        Memory mem3;
        M6502  cpu3(&mem3);
        pom2::SnapshotReader r(full1.data(), full1.size());
        assert(r.good());
        const auto res = pom2::restoreMachineState(r, cpu3, mem3);
        assert(res.ok);   // SLOT6 simply skipped
    }

    // (4) A card ignores a foreign / truncated blob (magic + version guard).
    {
        DiskIICard cardC(6);
        std::vector<uint8_t> before;
        cardC.appendSnapshotState(before);
        const uint8_t junk[4] = { 'X', 'X', 0x01, 0x02 };
        cardC.loadSnapshotState(junk, sizeof(junk));     // wrong magic + too short
        std::vector<uint8_t> after;
        cardC.appendSnapshotState(after);
        assert(after == before && "card mutated on a foreign blob");
    }

    std::printf("Rewind slot state: OK (DiskII round-trip + SLOT6 wiring + guards)\n");
    return 0;
}

// Rewind disk-write test (Phase 6 — writes are undone on a rewind).
//
// DiskIICard's snapshot (v2) carries the writable nibble track buffers for
// loaded, non-write-protected, non-WOZ disks, so a rewind reverts a disk
// write. This pins:
//   1. DiskImage media COW: writeNibbleAt then loadMediaSnapshot reverts the
//      nibble (the underlying mechanism);
//   2. end-to-end through the card + MachineSnapshot: write to the disk via
//      the controller, then restoring an earlier full-machine snapshot undoes
//      the write (re-capture is byte-identical to the pre-write capture);
//   3. an empty / no-disk drive adds no media (just a 1-byte present flag).

#include "DiskIICard.h"
#include "DiskImage.h"
#include "M6502.h"
#include "MachineSnapshot.h"
#include "Memory.h"
#include "SnapshotIO.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <vector>

namespace {

// A raw 143360-byte DOS 3.3 sector image loads (and nibblizes) regardless of
// content — we only need structurally-loadable, writable bytes.
std::string writeSyntheticDsk(const char* name)
{
    std::vector<uint8_t> buf(143360);
    for (size_t i = 0; i < buf.size(); ++i)
        buf[i] = static_cast<uint8_t>((i * 7 + 0x11) & 0xFF);
    const auto path = std::filesystem::temp_directory_path() / name;
    std::FILE* f = std::fopen(path.string().c_str(), "wb");
    assert(f);
    std::fwrite(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    return path.string();
}

std::vector<uint8_t> fullCapture(M6502& cpu, Memory& mem)
{
    std::vector<uint8_t> blob;
    pom2::SnapshotWriter w(blob);
    pom2::captureMachineState(w, cpu, mem, /*includeSlots=*/true);
    return blob;
}

// Drive a short write burst through the controller at the current head: motor
// on, drive 0, write mode, then a run of Q6H data stores paced by cycles.
void driveWrite(SlotPeripheral* c)
{
    c->deviceSelectRead(0x9);    // motor on
    c->deviceSelectRead(0xA);    // select drive 0
    c->deviceSelectRead(0xF);    // Q7H — write mode
    for (int i = 0; i < 64; ++i) {
        c->deviceSelectWrite(0x0D, static_cast<uint8_t>(0x80 | (i & 0x7F)));  // Q6H store
        c->advanceCycles(32);
    }
    c->deviceSelectRead(0xE);    // back to read mode
    c->deviceSelectRead(0xC);
}

}  // namespace

int main()
{
    const std::string dsk = writeSyntheticDsk("pom2_rewind_diskwrite.dsk");

    // (1) DiskImage media COW reverts a nibble write.
    {
        DiskImage img;
        assert(img.loadFile(dsk));
        assert(!img.isFileWriteProtected() && !img.isWoz());   // can be written in-memory

        std::vector<uint8_t> mediaA;
        img.appendMediaSnapshot(mediaA);
        assert(mediaA.size() == DiskImage::kMediaSnapshotBytes);

        const uint8_t orig = img.nibbleAt(5, 100);
        img.writeNibbleAt(5, 100, static_cast<uint8_t>(orig ^ 0x55));
        assert(img.nibbleAt(5, 100) == static_cast<uint8_t>(orig ^ 0x55));  // write took

        img.loadMediaSnapshot(mediaA.data(), mediaA.size());
        assert(img.nibbleAt(5, 100) == orig);                               // write undone
    }

    // (2) End-to-end: write via the card, then rewind undoes it.
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        auto* card = static_cast<DiskIICard*>(mem.slotBus().peripheral(6));
        assert(card->insertDisk(0, dsk));

        const std::vector<uint8_t> fullPre = fullCapture(cpu, mem);
        // The v2 blob must actually carry the media (≫ the ~80-byte mechanical
        // state) for a writable disk.
        assert(fullPre.size() > DiskImage::kMediaSnapshotBytes);

        driveWrite(card);
        const std::vector<uint8_t> fullPost = fullCapture(cpu, mem);
        assert(fullPost != fullPre && "disk write did not change the captured media");

        // Rewind: restore the pre-write snapshot, re-capture — the write is gone.
        pom2::SnapshotReader r(fullPre.data(), fullPre.size());
        assert(r.good());
        assert(pom2::restoreMachineState(r, cpu, mem).ok);
        const std::vector<uint8_t> fullAfter = fullCapture(cpu, mem);
        assert(fullAfter == fullPre && "rewind did not undo the disk write");
    }

    // (3) No disk → no media (cheap blob).
    {
        Memory mem;
        M6502  cpu(&mem);
        mem.slotBus().plug(6, std::make_unique<DiskIICard>(6));
        std::vector<uint8_t> blob;
        mem.slotBus().peripheral(6)->appendSnapshotState(blob);
        assert(blob.size() < 256 && "empty drive should not append media");
    }

    std::filesystem::remove(dsk);
    std::printf("Rewind disk write: OK (media COW + card write-undo + empty-drive guard)\n");
    return 0;
}

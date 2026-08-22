// Two-phase mount — pins DiskIICard::prepareDisk / installDisk.
//
// The split exists so a UI caller can read and decode a disk image WITHOUT
// holding `stateMutex`, which the CPU worker takes every 4096-cycle chunk and
// the UI thread takes to paint every frame. `insertDisk(drive, path)` gave the
// caller no way to do that: it flushes, reads, decodes and installs in one
// call, so a mount froze the machine and the window together for the length of
// the read (12.8 ms for a 32 MB image on a warm cache, most of a PAL frame).
//
// What needs pinning is not the speed — it is the correctness the split put at
// risk. Phase 1 reads BEFORE phase 2 flushes, which inverts the order
// insertDisk had, and that inversion has exactly one victim: re-inserting a
// file the guest has unsaved writes to. Read first and the prepared image
// holds pre-flush bytes; install it and the guest's writes are silently rolled
// back — the medium reverts, and nothing anywhere reports an error. Case 3 is
// that scenario, and it is the reason installDisk detects the collision and
// re-reads under the lock instead of trusting the prepared image.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

fs::path tmpPath(const std::string& leaf)
{
    return fs::temp_directory_path() / leaf;
}

// A raw .nib image: saveDirty writes the nibble buffers back verbatim, so a
// surviving guest write shows up as a byte difference. A .dsk would round-trip
// through the GCR decoder, which repairs the mangled sector from the file's
// own content and would hide exactly what these cases are looking for.
void writeNibImage(const fs::path& p, uint8_t fill)
{
    std::vector<uint8_t> bytes(
        static_cast<std::size_t>(DiskImage::kTracks) *
        DiskImage::kNibblesPerTrack, fill);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

std::vector<uint8_t> readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f),
                                std::istreambuf_iterator<char>());
}

// Drive a write through the card's nibble gate, the way the guest would:
// motor on, Q7 high (write), latch valid GCR bytes across a sector's worth of
// track, Q7 low. Same sequence disk_media_state uses.
void dirtyThroughTheCard(DiskIICard& card)
{
    card.deviceSelectRead(0x9);            // motor on
    card.deviceSelectWrite(0xF, 0x00);     // Q7 high → write mode
    for (int i = 0; i < 4000; ++i) {
        card.deviceSelectWrite(0xC, 0x96); // latch a valid GCR nibble
        card.advanceCycles(32);
    }
    card.deviceSelectRead(0xE);            // Q7 low → read mode
}

// Step the head outward. Each magnet transition is a half-track, so two per
// track. Self-verifying: the caller asserts on the resulting track number.
void stepOutTracks(DiskIICard& card, int tracks)
{
    int phase = 0;
    for (int i = 0; i < tracks * 2; ++i) {
        phase = (phase + 1) & 3;
        card.deviceSelectRead(static_cast<uint8_t>(phase * 2 + 1));  // magnet on
        card.advanceCycles(2000);
        card.deviceSelectRead(static_cast<uint8_t>(phase * 2));      // magnet off
        card.advanceCycles(2000);
    }
}

// Bytes of one track inside a .nib file, which stores tracks verbatim and
// back to back.
std::vector<uint8_t> trackSlice(const std::vector<uint8_t>& nib, int track)
{
    const std::size_t n = DiskImage::kNibblesPerTrack;
    const std::size_t at = static_cast<std::size_t>(track) * n;
    if (at + n > nib.size()) return {};
    return std::vector<uint8_t>(nib.begin() + static_cast<std::ptrdiff_t>(at),
                                nib.begin() + static_cast<std::ptrdiff_t>(at + n));
}

// ── 1. prepare + install is equivalent to insertDisk ─────────────────────
void testPrepareThenInstallMounts()
{
    const fs::path img = tmpPath("pom2_two_phase_basic.nib");
    writeNibImage(img, 0xFF);

    DiskIICard card(6);
    DiskImage  prepared;
    std::string error = "not cleared";

    // Phase 1 touches no card state — the point of the split.
    assert(DiskIICard::prepareDisk(img.string(), /*writeBack=*/false,
                                   prepared, error));
    assert(error.empty());
    assert(prepared.isLoaded());
    assert(!card.isDiskLoaded(0) && "phase 1 must not have mounted anything");

    // Phase 2 adopts it.
    assert(card.installDisk(0, std::move(prepared)));
    assert(card.isDiskLoaded(0));
    assert(card.getDiskPath(0) == img.string());

    std::printf("[ OK ] prepare + install mounts the image\n");
    std::error_code ec;
    fs::remove(img, ec);
}

// ── 2. the failure paths report rather than half-mount ───────────────────
void testFailurePaths()
{
    DiskIICard  card(6);
    DiskImage   prepared;
    std::string error;

    // A file that is not there fails in phase 1, with a message, and leaves
    // the drive alone.
    assert(!DiskIICard::prepareDisk(tmpPath("pom2_two_phase_absent.nib").string(),
                                    false, prepared, error));
    assert(!error.empty() && "phase 1 failure must say why");
    assert(!card.isDiskLoaded(0));

    // An image that was never prepared is refused by phase 2 rather than
    // installed as an empty medium.
    DiskImage unprepared;
    assert(!card.installDisk(0, std::move(unprepared)));
    assert(!card.isDiskLoaded(0));

    // An out-of-range drive is refused by both halves' shared guard.
    const fs::path img = tmpPath("pom2_two_phase_range.nib");
    writeNibImage(img, 0xFF);
    DiskImage ok;
    assert(DiskIICard::prepareDisk(img.string(), false, ok, error));
    assert(!card.installDisk(7, std::move(ok)));

    std::printf("[ OK ] failure paths report and leave the drive untouched\n");
    std::error_code ec;
    fs::remove(img, ec);
}

// ── 3. THE case: re-inserting a file the guest has written to ────────────
// This is the regression the split could have introduced. Phase 1 reads the
// file while the card still holds unsaved writes to that same file; phase 2
// then flushes them. Installing the prepared image at that point would put the
// pre-flush bytes back and roll the guest's writes away, silently.
void testSameFileWithUnsavedWritesIsNotRolledBack()
{
    const fs::path img = tmpPath("pom2_two_phase_samefile.nib");
    writeNibImage(img, 0xFF);
    const std::vector<uint8_t> pristine = readFile(img);

    DiskIICard card(6);
    card.setWriteBackEnabled(true);
    assert(card.insertDisk(0, img.string()));

    dirtyThroughTheCard(card);
    assert(card.hasUnsavedChanges(0) &&
           "test setup failed: no write reached the media");

    // Phase 1 now reads the file as it is ON DISK — still pristine, because
    // the guest's writes live only in the card's nibble buffers.
    DiskImage   prepared;
    std::string error;
    assert(DiskIICard::prepareDisk(img.string(), /*writeBack=*/true,
                                   prepared, error));

    // Phase 2. installDisk must notice that the outgoing medium is this same
    // file with unsaved changes, flush it, and re-read — not install the
    // stale image it was handed.
    assert(card.installDisk(0, std::move(prepared)));
    assert(card.isDiskLoaded(0));

    // The guest's writes reached the file...
    const std::vector<uint8_t> onDisk = readFile(img);
    assert(onDisk.size() == pristine.size());
    assert(onDisk != pristine &&
           "the guest's writes were rolled back by the two-phase mount");

    // ...and — the assertion that actually discriminates — the medium the card
    // is NOW holding has to be the flushed file, not the stale bytes phase 1
    // captured. Both cases look identical until the guest writes again: the
    // next flush writes the WHOLE mounted medium back, so a stale image
    // silently reverts track 0 to its pristine content.
    //
    // So: step to another track, write there, flush, and check that track 0
    // still carries the first burst. This is precisely the user-visible harm
    // — save a file, re-insert the disk, save a second file, and the first one
    // is gone.
    assert(!card.hasUnsavedChanges(0));
    stepOutTracks(card, 3);
    assert(card.getCurrentTrack(0) == 3 && "test setup failed: head did not step");
    dirtyThroughTheCard(card);
    assert(card.hasUnsavedChanges(0));
    assert(card.flushPendingWrites());

    const std::vector<uint8_t> afterSecondSave = readFile(img);
    assert(trackSlice(afterSecondSave, 0) == trackSlice(onDisk, 0) &&
           "a stale mounted image rolled track 0 back over the guest's writes");
    assert(trackSlice(afterSecondSave, 0) != trackSlice(pristine, 0) &&
           "track 0 lost the first write burst");

    std::printf("[ OK ] same-file re-insert keeps the guest's writes\n");
    std::error_code ec;
    fs::remove(img, ec);
}

// ── 4. a DIFFERENT file still flushes the outgoing medium ────────────────
void testOutgoingMediumIsFlushed()
{
    const fs::path a = tmpPath("pom2_two_phase_out_a.nib");
    const fs::path b = tmpPath("pom2_two_phase_out_b.nib");
    writeNibImage(a, 0xFF);
    writeNibImage(b, 0xAA);
    const std::vector<uint8_t> pristineA = readFile(a);

    DiskIICard card(6);
    card.setWriteBackEnabled(true);
    assert(card.insertDisk(0, a.string()));
    dirtyThroughTheCard(card);
    assert(card.hasUnsavedChanges(0));

    DiskImage   prepared;
    std::string error;
    assert(DiskIICard::prepareDisk(b.string(), true, prepared, error));
    assert(card.installDisk(0, std::move(prepared)));

    assert(card.getDiskPath(0) == b.string());
    assert(readFile(a) != pristineA &&
           "the outgoing medium was dropped without its write-back");

    std::printf("[ OK ] swapping media still flushes the outgoing one\n");
    std::error_code ec;
    fs::remove(a, ec);
    fs::remove(b, ec);
}

} // namespace

int main()
{
    testPrepareThenInstallMounts();
    testFailurePaths();
    testSameFileWithUnsavedWritesIsNotRolledBack();
    testOutgoingMediumIsFlushed();
    std::printf("two_phase_mount: all assertions passed\n");
    return 0;
}

// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Regression pins for the disk / floppy MANAGEMENT layer — the state that
// lives around the LSS data path rather than inside it. Five defects, each
// with its own case below:
//
//   1. `serving13_` / `useBitLss` were latched on insert and never cleared
//      on eject, so removing a 13-sector disk left the card serving the
//      341-0009 13-sector boot PROM at $Cn00 to whatever came next.
//   2. A host-read-only image mounted as fully writable: the guest wrote
//      happily and `saveDirty()` then REPLACED the read-only file anyway
//      (writeFileAtomic renames a sibling temp; POSIX rename only needs
//      write permission on the directory).
//   3. Write-back was flushed on eject and on swap but NOT on teardown,
//      so quitting POM2 dropped every sector written since the last eject.
//   4. `writeFileAtomic` didn't carry the original file's permissions
//      across the rename — a write-back rewrote the image's mode to the
//      process umask default.
//   5. `onReset()` cleared `motorOn` without telling the mechanical-sound
//      sink, so a Ctrl-Reset mid-read left the motor hum looping forever.

#include "DiskIICard.h"
#include "DiskImage.h"
#include "FloppySoundSink.h"

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

const char* firstExisting(std::initializer_list<const char*> candidates)
{
    for (const char* c : candidates) {
        std::ifstream f(c, std::ios::binary);
        if (f) return c;
    }
    return nullptr;
}

std::string tmpPath(const char* leaf)
{
    return (std::filesystem::temp_directory_path() / leaf).string();
}

void writeFile(const std::string& path, const std::vector<uint8_t>& bytes)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    assert(f && "cannot create scratch image");
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
}

// A 143 360-byte DOS 3.3 image and a 116 480-byte 13-sector image. Contents
// don't matter — only the size, which is what detectFormat routes on.
std::string make16SectorImage()
{
    const std::string p = tmpPath("pom2_media_state_16s.dsk");
    writeFile(p, std::vector<uint8_t>(DiskImage::kBytesPerImage, 0x00));
    return p;
}

std::string make13SectorImage()
{
    const std::string p = tmpPath("pom2_media_state_13s.d13");
    writeFile(p, std::vector<uint8_t>(DiskImage::kBytesPerImage13, 0x00));
    return p;
}

// ── 1. eject un-latches the 13-sector boot PROM ─────────────────────────
void testEjectClears13SectorLatch()
{
    const char* rom13 = firstExisting({ "roms/disk2_13.rom",
                                        "../roms/disk2_13.rom",
                                        "../../roms/disk2_13.rom" });
    if (!rom13) {
        std::printf("[SKIP] roms/disk2_13.rom not found — "
                    "skipping 13-sector latch test\n");
        return;
    }

    DiskIICard card(6);
    assert(card.loadBootRom13(rom13));

    // Baseline: the embedded 16-sector P5A PROM.
    std::vector<uint8_t> prom16(256);
    for (int i = 0; i < 256; ++i)
        prom16[i] = card.slotRomRead(static_cast<uint8_t>(i));

    const std::string img13 = make13SectorImage();
    const std::string img16 = make16SectorImage();
    assert(card.insertDisk(0, img13));

    // With the 13-sector disk in, $Cn00 must come out of the 341-0009 PROM.
    bool differs = false;
    for (int i = 0; i < 256 && !differs; ++i)
        differs = card.slotRomRead(static_cast<uint8_t>(i)) != prom16[i];
    assert(differs && "13-sector disk did not select the 341-0009 boot PROM");

    // Put a stock 16-sector disk in drive 2 and pull the 13-sector one.
    assert(card.insertDisk(1, img16));
    card.ejectDisk(0);

    // THE BUG: serving13_ stayed set, so the 16-sector disk still booted
    // against the 13-sector PROM.
    for (int i = 0; i < 256; ++i) {
        assert(card.slotRomRead(static_cast<uint8_t>(i)) == prom16[i] &&
               "13-sector boot PROM still latched after eject");
    }

    std::filesystem::remove(img13);
    std::filesystem::remove(img16);
    std::printf("[ OK ] eject clears the 13-sector boot-PROM latch\n");
}

// ── 2. host read-only image mounts write-protected ──────────────────────
void testHostReadOnlyMountsWriteProtected()
{
    namespace fs = std::filesystem;
    const std::string img = make16SectorImage();

    // Writable: physical WP off (the write-back toggle is a separate gate).
    {
        DiskImage d;
        assert(d.loadFile(img));
        assert(!d.isFileWriteProtected());
    }

    fs::permissions(img, fs::perms::owner_read | fs::perms::group_read |
                         fs::perms::others_read,
                    fs::perm_options::replace);

    // Running as root defeats the mode bits entirely — the probe opens the
    // file regardless, so there is nothing to assert.
    std::ofstream rootProbe(img, std::ios::in | std::ios::out | std::ios::binary);
    const bool modeEnforced = !rootProbe;
    rootProbe.close();

    if (modeEnforced) {
        DiskImage d;
        assert(d.loadFile(img));
        assert(d.isFileWriteProtected() &&
               "chmod-read-only image mounted as writable");
        // …and physical WP inhibits the in-memory write, so saveDirty can
        // never reach the user's file.
        d.setWriteBackEnabled(true);
        d.writeNibbleAt(0, 0, 0xD5);
        assert(!d.hasUnsavedChanges());
        std::printf("[ OK ] host read-only image mounts write-protected\n");
    } else {
        std::printf("[SKIP] running with mode bits bypassed (root?) — "
                    "skipping read-only mount test\n");
    }

    fs::permissions(img, fs::perms::owner_all, fs::perm_options::add);
    fs::remove(img);
}

// ── 3. teardown flushes pending write-back ──────────────────────────────
void testFlushOnTeardown()
{
    // A raw .nib image: saveDirty writes the nibble buffers back verbatim,
    // so any surviving write shows up as a byte difference. (A .dsk would
    // round-trip through the GCR decoder, which repairs the mangled sector
    // from the pre-filled file content and hides the change.)
    const std::string img = tmpPath("pom2_media_state_flush.nib");
    writeFile(img, std::vector<uint8_t>(
        static_cast<std::size_t>(DiskImage::kTracks) *
        DiskImage::kNibblesPerTrack, 0xFF));

    // The assertion: a dirty image is written out by the DESTRUCTOR, with
    // no eject and no explicit save anywhere in sight.
    std::vector<uint8_t> before;
    {
        std::ifstream f(img, std::ios::binary);
        before.assign(std::istreambuf_iterator<char>(f),
                      std::istreambuf_iterator<char>());
    }

    {
        DiskIICard card(6);
        card.setWriteBackEnabled(true);
        assert(card.insertDisk(0, img));
        // Drive the write through the legacy nibble gate: motor on, write
        // mode, latch a byte, then advance enough cycles to flush nibbles
        // over a whole sector's worth of the track.
        card.deviceSelectRead(0x9);            // motor on
        card.deviceSelectWrite(0xF, 0x00);     // Q7 high → write mode
        for (int i = 0; i < 4000; ++i) {
            card.deviceSelectWrite(0xC, 0x96); // latch a valid GCR nibble
            card.advanceCycles(32);
        }
        card.deviceSelectRead(0xE);            // Q7 low → read mode
        assert(card.hasUnsavedChanges(0) &&
               "test setup failed: no write reached the media");
        // No eject, no explicit save — just let the card die.
    }

    std::vector<uint8_t> after;
    {
        std::ifstream f(img, std::ios::binary);
        after.assign(std::istreambuf_iterator<char>(f),
                     std::istreambuf_iterator<char>());
    }
    assert(after.size() == before.size());
    assert(after != before &&
           "pending write-back was dropped on teardown");

    std::filesystem::remove(img);
    std::printf("[ OK ] pending write-back is flushed on card teardown\n");
}

// ── 4. write-back preserves the image's permissions ─────────────────────
void testSavePreservesPermissions()
{
    namespace fs = std::filesystem;
    const std::string img = make16SectorImage();

    // A deliberately non-default mode: owner rw + group rw, no world access.
    const fs::perms want = fs::perms::owner_read  | fs::perms::owner_write |
                           fs::perms::group_read  | fs::perms::group_write;
    fs::permissions(img, want, fs::perm_options::replace);

    DiskImage d;
    assert(d.loadFile(img));
    assert(!d.isFileWriteProtected());
    d.setWriteBackEnabled(true);
    d.writeNibbleAt(3, 17, 0xD5);
    assert(d.hasUnsavedChanges());
    assert(d.saveDirty());

    const fs::perms got = fs::status(img).permissions() & fs::perms::mask;
    assert(got == want && "saveDirty reset the image's permissions");

    fs::permissions(img, fs::perms::owner_all, fs::perm_options::add);
    fs::remove(img);
    std::printf("[ OK ] write-back preserves the image file's permissions\n");
}

// ── 5. reset stops the motor sound ──────────────────────────────────────
struct CountingSound : FloppySoundSink
{
    int  motorOnCalls  = 0;
    int  motorOffCalls = 0;
    void motor(bool on, bool /*withDisk*/) override
    {
        if (on) ++motorOnCalls; else ++motorOffCalls;
    }
    void step(int, uint64_t) override {}
    void click() override {}
};

void testResetStopsMotorSound()
{
    const std::string img = make16SectorImage();

    DiskIICard card(6);
    CountingSound snd;
    card.setFloppySound(&snd);
    assert(card.insertDisk(0, img));

    card.deviceSelectRead(0x9);      // $C0n9 — motor on
    assert(card.isMotorOn());
    assert(snd.motorOnCalls == 1);
    assert(snd.motorOffCalls == 0);

    card.onReset();                  // Ctrl-Reset while the drive spins
    assert(!card.isMotorOn());
    assert(snd.motorOffCalls == 1 &&
           "reset stopped the motor without silencing the spin loop");

    std::filesystem::remove(img);
    std::printf("[ OK ] reset emits the matching motor-off to the sound sink\n");
}

}  // namespace

int main()
{
    testEjectClears13SectorLatch();
    testHostReadOnlyMountsWriteProtected();
    testFlushOnTeardown();
    testSavePreservesPermissions();
    testResetStopsMotorSound();
    std::printf("disk_media_state OK\n");
    return 0;
}

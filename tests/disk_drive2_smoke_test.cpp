// Drive 2 smoke test for DiskIICard.
//
// Pins the dual-drive routing wired in for the MAME-style $C0nA / $C0nB
// drive_select. Exercises:
//
//   1. Independent insert into drives 1 and 2 with two synthetic .nib
//      images that carry a DIFFERENT marker byte at track 0 byte 64.
//   2. After a $C0EA access (drive 1 select) the LSS surfaces drive 1's
//      marker; after $C0EB (drive 2 select) it surfaces drive 2's.
//   3. Each drive's head position is independent — stepping the head
//      while drive 1 is selected does not move drive 2's head, and vice
//      versa.
//   4. The legacy 32-cycle gate path (no P6 PROM loaded) honours the
//      same drive_select switching, so software running on a controller
//      built without `roms/diskii_p6.rom` still picks up the second drive.
//
// All synthetic — no Apple II ROM or real disk image required.

#include "DiskIICard.h"
#include "DiskImage.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

// Build a synthetic .nib track buffer (35 × 6656 bytes) with a single
// `D5 AA 96 <marker>` quadruple at track 0 offset 64. Returns the path to
// the temporary file.
std::string makeSyntheticNib(uint8_t marker, const char* tag) {
    constexpr int kTracks = DiskImage::kTracks;
    constexpr int kBytesPerTrack = DiskImage::kNibblesPerTrack;
    std::vector<uint8_t> img(static_cast<size_t>(kTracks) * kBytesPerTrack,
                             0xFF);
    constexpr int kProloguePos = 64;
    img[kProloguePos + 0] = 0xD5;
    img[kProloguePos + 1] = 0xAA;
    img[kProloguePos + 2] = 0x96;
    img[kProloguePos + 3] = marker;

    const auto tmp = fs::temp_directory_path()
        / (std::string("pom2_drive2_") + tag + ".nib");
    std::FILE* f = std::fopen(tmp.string().c_str(), "wb");
    assert(f && "failed to open temp .nib for writing");
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    return tmp.string();
}

// Drive the controller for `cpuCycles` worth of "spin", reading $C0EC at
// every 8-cycle interval and recording bytes that arrive with bit 7 set
// (= valid GCR nibble). De-duplicates consecutive reads of the same
// latch value. Mirrors the spin loop used by diskii_lss_smoke.
std::vector<uint8_t> spinAndCollect(DiskIICard& card,
                                    int cpuCycles,
                                    size_t maxBytes)
{
    std::vector<uint8_t> out;
    out.reserve(maxBytes);
    constexpr int kCyclesPerRead = 8;

    // Make sure motor's running and we're in read mode (Q6L+Q7L). The
    // caller is responsible for the drive_select access *before* the
    // spin so the LSS catches the new flux events from the right drive.
    card.deviceSelectRead(0x9);   // motor on
    card.deviceSelectRead(0xE);   // Q7L
    card.deviceSelectRead(0xC);   // Q6L

    int spent = 0;
    while (spent < cpuCycles && out.size() < maxBytes) {
        card.advanceCycles(kCyclesPerRead);
        spent += kCyclesPerRead;
        const uint8_t b = card.deviceSelectRead(0xC);
        if (b & 0x80) {
            if (out.empty() || out.back() != b) out.push_back(b);
        }
    }
    return out;
}

bool containsPrologueWithMarker(const std::vector<uint8_t>& bytes,
                                uint8_t expectedMarker)
{
    for (size_t i = 0; i + 3 < bytes.size(); ++i) {
        if (bytes[i] == 0xD5 && bytes[i+1] == 0xAA && bytes[i+2] == 0x96
            && bytes[i+3] == expectedMarker) {
            return true;
        }
    }
    return false;
}

std::string findFirst(std::initializer_list<const char*> candidates) {
    for (const char* p : candidates) {
        std::error_code ec;
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

// Test 1: dual-drive routing under the bit-level LSS path. The card has
// drive 1 = marker $A1, drive 2 = marker $B2; selecting drive 2 must
// surface $B2 immediately after the prologue.
bool testLssDualDriveRouting() {
    const std::string lssPath = findFirst({
        "../roms/diskii_p6.rom", "roms/diskii_p6.rom",
        "../../roms/diskii_p6.rom"
    });
    DiskIICard card;
    if (!lssPath.empty()) (void)card.loadLssRom(lssPath);

    const std::string nib1 = makeSyntheticNib(0xA1, "d1");
    const std::string nib2 = makeSyntheticNib(0xB2, "d2");
    if (!card.insertDisk(0, nib1)) {
        std::printf("FAIL: insertDisk(0): %s\n",
                    card.getLastError(0).c_str()); return false;
    }
    if (!card.insertDisk(1, nib2)) {
        std::printf("FAIL: insertDisk(1): %s\n",
                    card.getLastError(1).c_str()); return false;
    }
    if (!card.isDiskLoaded(0) || !card.isDiskLoaded(1)) {
        std::printf("FAIL: both drives should report loaded\n"); return false;
    }

    // Drive 1 first.
    card.deviceSelectRead(0xA);                // drive 1 select
    if (card.getActiveDrive() != 0) {
        std::printf("FAIL: getActiveDrive=%d after $C0EA\n",
                    card.getActiveDrive()); return false;
    }
    auto bytes = spinAndCollect(card, 1'000'000, 96);
    if (!containsPrologueWithMarker(bytes, 0xA1)) {
        std::printf("FAIL: drive 1 prologue+marker not seen in %zu bytes\n",
                    bytes.size()); return false;
    }

    // Drive 2 second.
    card.deviceSelectRead(0xB);                // drive 2 select
    if (card.getActiveDrive() != 1) {
        std::printf("FAIL: getActiveDrive=%d after $C0EB\n",
                    card.getActiveDrive()); return false;
    }
    bytes = spinAndCollect(card, 1'000'000, 96);
    if (!containsPrologueWithMarker(bytes, 0xB2)) {
        std::printf("FAIL: drive 2 prologue+marker not seen in %zu bytes\n",
                    bytes.size()); return false;
    }

    // And back to drive 1 — the data register must reset to drive 1's
    // marker, not stay stuck on drive 2's.
    card.deviceSelectRead(0xA);
    bytes = spinAndCollect(card, 1'000'000, 96);
    if (!containsPrologueWithMarker(bytes, 0xA1)) {
        std::printf("FAIL: drive 1 marker not re-seen after switch back\n");
        return false;
    }
    std::printf("[ OK ] LSS path routes drive_select through to images[]\n");
    return true;
}

// Test 2: independent head positions per drive. While drive 1 is selected
// step its head a few quarter-tracks via phase pulses; switch to drive 2
// and confirm drive 2's head is still at 0; then step drive 2's head and
// confirm drive 1's is still where we left it.
bool testIndependentHeadPositions() {
    DiskIICard card;
    const std::string nib1 = makeSyntheticNib(0xA1, "d1head");
    const std::string nib2 = makeSyntheticNib(0xB2, "d2head");
    assert(card.insertDisk(0, nib1));
    assert(card.insertDisk(1, nib2));

    // The LSS path's seekPhaseW only moves the head when active != IDLE;
    // make sure motor is on so the phase pokes actually take effect.
    card.deviceSelectRead(0x9);   // motor on (active = MODE_ACTIVE)

    // Drive 1: walk to half-track 4 by pulsing phase 0 → 1 → 2.
    card.deviceSelectRead(0xA);   // drive 1 select
    auto step = [&](int phase) {
        card.deviceSelectRead(static_cast<uint8_t>(phase * 2 + 1));   // phase on
        card.deviceSelectRead(static_cast<uint8_t>(phase * 2 + 0));   // phase off
    };
    step(1);   // 0 → 2
    step(2);   // 2 → 4

    const int d1AfterStep = card.getQuarterTrack(0);
    const int d2AfterStep = card.getQuarterTrack(1);
    if (d1AfterStep == 0) {
        std::printf("FAIL: drive 1 head did not move (qt=%d)\n", d1AfterStep);
        return false;
    }
    if (d2AfterStep != 0) {
        std::printf("FAIL: drive 2 head moved while drive 1 was selected"
                    " (qt=%d)\n", d2AfterStep);
        return false;
    }

    // Drive 2: select and walk *its* head.
    card.deviceSelectRead(0xB);   // drive 2 select
    step(1);                       // 0 → 2
    const int d1AfterDrive2Step = card.getQuarterTrack(0);
    const int d2AfterDrive2Step = card.getQuarterTrack(1);
    if (d2AfterDrive2Step == 0) {
        std::printf("FAIL: drive 2 head did not move (qt=%d)\n",
                    d2AfterDrive2Step); return false;
    }
    if (d1AfterDrive2Step != d1AfterStep) {
        std::printf("FAIL: drive 1 head shifted while drive 2 was selected"
                    " (was %d, now %d)\n",
                    d1AfterStep, d1AfterDrive2Step); return false;
    }
    std::printf("[ OK ] heads are independent (d1 qt=%d, d2 qt=%d)\n",
                d1AfterDrive2Step, d2AfterDrive2Step);
    return true;
}

// Test 3: legacy 32-cycle gate path also routes drive_select. Same as
// test 1 but with no P6 PROM loaded — useBitLss stays false, so the
// path through deviceSelectRead/legacyAdvance must consult activeDrive
// when picking the image to read from.
bool testLegacyDualDriveRouting() {
    DiskIICard card;     // no loadLssRom call → legacy gate

    const std::string nib1 = makeSyntheticNib(0xA1, "d1leg");
    const std::string nib2 = makeSyntheticNib(0xB2, "d2leg");
    assert(card.insertDisk(0, nib1));
    assert(card.insertDisk(1, nib2));

    card.deviceSelectRead(0xA);
    auto bytes = spinAndCollect(card, 250'000, 64);
    if (!containsPrologueWithMarker(bytes, 0xA1)) {
        std::printf("FAIL: legacy gate drive 1 prologue+marker not seen"
                    " in %zu bytes\n", bytes.size()); return false;
    }
    card.deviceSelectRead(0xB);
    bytes = spinAndCollect(card, 250'000, 64);
    if (!containsPrologueWithMarker(bytes, 0xB2)) {
        std::printf("FAIL: legacy gate drive 2 prologue+marker not seen"
                    " in %zu bytes\n", bytes.size()); return false;
    }
    std::printf("[ OK ] legacy gate path routes drive_select correctly\n");
    return true;
}

}  // namespace

int main() {
    bool ok = true;
    ok &= testLssDualDriveRouting();
    ok &= testIndependentHeadPositions();
    ok &= testLegacyDualDriveRouting();
    if (ok) {
        std::printf("disk_drive2_smoke OK\n");
        return 0;
    }
    std::printf("disk_drive2_smoke FAILED\n");
    return 1;
}

// Pins the content-driven skew sniff that decides DOS 3.3 vs ProDOS sector
// order from file content, overriding the extension when they disagree.
//
// Three cases:
//   1. cc65-Chess.po — `.po` extension but DOS-3.3-skewed payload. Must
//      detect via vol-header sniff at the .dsk position (file offset
//      0xB00) and downgrade to Dos33.
//   2. ProDOS_2_4_3.po — `.po` extension with matching ProDOS skew.
//      Must stay ProDOS without override.
//   3. dos33_master.dsk — `.dsk` extension with DOS skew. Must stay
//      Dos33 with no spurious override.
//
// Canonical fixture paths (build/tests/ cwd):
//   ../../disks/dsk/cc65-Chess.po
//   ../../disks/dsk/dos33_master.dsk
//   ../../disks/ProDOS_2_4_3.po       (kept at disks/ root historically)
//
// Earlier versions of this test treated a missing fixture as "not a
// failure" — combined with a probe list that didn't cover the dsk/
// subdirectory, that silently green-passed against cc65-Chess.po for
// the entire skew-sniff series of commits. Missing-fixture is now a
// hard failure for the three named real cases; the synthetic spoof
// negative below stays unconditional.
//
// Also pins the name-character validation in `looksLikeVolHeader`: a
// synthetic image whose alternate-skew offset has the right header byte
// but bogus name characters must NOT trigger an override.
//
// Failure modes this test guards against:
//   - Sniff regression that misclassifies cc65-Chess.po (hangs the boot)
//   - Over-eager override on a real DOS disk that happens to have a $Fx
//     byte at offset 0x404 in its catalog (random false positive)
//   - Probe list rot when the fixture pack is reorganised into subdirs
//     (caught now by the hard failure on missing fixtures)

#include "DiskImage.h"

#include <array>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool runRealDiskCase(const char* fixtureName,
                     DiskImage::SectorOrder expected,
                     const char* label)
{
    // ctest cwd is build/tests/. The fixture pack is split: WOZ images
    // under disks/woz/, sector images under disks/dsk/, with some legacy
    // disks/ and disks2/ roots still in use. Probe all variants so the
    // test locates fixtures regardless of layout changes.
    static const char* prefixes[] = {
        "../../disks/",      "../../disks/dsk/",  "../../disks/woz/",
        "../../disks2/",
        "disks/",            "disks/dsk/",        "disks/woz/",
        "disks2/",
    };
    std::string path;
    for (const char* pfx : prefixes) {
        const std::string candidate = std::string(pfx) + fixtureName;
        std::ifstream f(candidate, std::ios::binary);
        if (f) { path = candidate; break; }
    }
    if (path.empty()) {
        std::fprintf(stderr,
            "%s: required fixture %s not found under disks/, disks/dsk/, "
            "disks/woz/, or disks2/ — failing loud rather than silently "
            "green-passing (fix the fixture or extend the probe list)\n",
            label, fixtureName);
        return false;
    }

    DiskImage img;
    if (!img.loadFile(path)) {
        std::fprintf(stderr, "%s: loadFile(%s) failed: %s\n", label,
                     path.c_str(), img.getLastError().c_str());
        return false;
    }
    const auto got = img.getSectorOrder();
    if (got != expected) {
        std::fprintf(stderr,
            "%s: sector order mismatch for %s (got %s, want %s)\n",
            label, path.c_str(),
            got == DiskImage::SectorOrder::ProDOS ? "ProDOS" : "Dos33",
            expected == DiskImage::SectorOrder::ProDOS ? "ProDOS" : "Dos33");
        return false;
    }
    return true;
}

// Synthesise a 143360-byte file where the alternate-skew vol-header
// offset has a byte pattern that LOOKS like a vol dir entry (prev=0,
// next=3, storage_type=$F, name_length=4) but whose 4 name bytes are
// $20 $20 $20 $20 — spaces, not valid ProDOS name chars. Pre-name-char
// validation this would have spoofed an override; post-fix it must not.
bool runSpoofNegativeCase()
{
    std::vector<uint8_t> buf(143360, 0);

    // ProDOS boot block at offset 0 so the file looks superficially like
    // a ProDOS image (matches the canonical 01 38 B0 03 4C signature the
    // older sniff used).
    static const uint8_t kProdosBoot[5] = {0x01, 0x38, 0xB0, 0x03, 0x4C};
    std::memcpy(buf.data(), kProdosBoot, sizeof(kProdosBoot));

    // The PROPER ProDOS position (file offset 0x400) has nothing — so
    // prodosVolHere is false.
    //
    // The ALTERNATE DOS-skew position (file offset 0xB00) has bytes that
    // would have passed the old predicate (storage type $F, name length
    // 4) but the name bytes are spaces — fails the new char check.
    buf[0xB00] = 0x00;        // prev_block lo
    buf[0xB01] = 0x00;        // prev_block hi
    buf[0xB02] = 0x03;        // next_block lo
    buf[0xB03] = 0x00;        // next_block hi
    buf[0xB04] = 0xF4;        // storage_type=$F, name_length=4
    buf[0xB05] = 0x20;        // name[0] = ' ' — bogus
    buf[0xB06] = 0x20;
    buf[0xB07] = 0x20;
    buf[0xB08] = 0x20;

    const std::string path = "skew_spoof_synthetic.po";
    std::ofstream wf(path, std::ios::binary | std::ios::trunc);
    if (!wf) {
        std::fprintf(stderr, "spoof: cannot create temp file\n");
        return false;
    }
    wf.write(reinterpret_cast<const char*>(buf.data()),
             static_cast<std::streamsize>(buf.size()));
    wf.close();

    DiskImage img;
    const bool ok = img.loadFile(path);
    std::remove(path.c_str());
    if (!ok) {
        std::fprintf(stderr, "spoof: load failed: %s\n",
                     img.getLastError().c_str());
        return false;
    }
    // The file has `.po` extension and no valid ProDOS vol header at
    // either position. With the strengthened sniff, the alt-skew offset
    // does NOT pass the name-char check, so we keep the ProDOS default
    // (per extension). Pre-fix behavior would have overridden to Dos33
    // incorrectly because the alt-position byte $F4 + plausible header
    // shape would have passed the laxer predicate.
    if (img.getSectorOrder() != DiskImage::SectorOrder::ProDOS) {
        std::fprintf(stderr,
            "spoof: sniff false-positive — got Dos33, want ProDOS "
            "(name bytes are spaces, not ProDOS chars)\n");
        return false;
    }
    return true;
}

}  // namespace

int main()
{
    bool ok = true;
    ok &= runRealDiskCase("cc65-Chess.po",
                          DiskImage::SectorOrder::Dos33,
                          "cc65-Chess.po");
    ok &= runRealDiskCase("ProDOS_2_4_3.po",
                          DiskImage::SectorOrder::ProDOS,
                          "ProDOS_2_4_3.po");
    ok &= runRealDiskCase("dos33_master.dsk",
                          DiskImage::SectorOrder::Dos33,
                          "dos33_master.dsk");
    ok &= runSpoofNegativeCase();

    if (!ok) return 1;
    std::printf("disk_skew_sniff_smoke OK: 3 real disks + spoof negative\n");
    return 0;
}

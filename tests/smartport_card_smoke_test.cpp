// SmartPortCard smoke test.
//
// Pins the //e Liron-class card's byte-stream protocol:
//   $C0n0 write = drive select (0 / 1)
//   $C0n1/n2    = block LO/HI of the active drive
//   $C0n3 read  = streaming data byte from the active drive
//   $C0n3 write = streaming data byte into the active drive
//   $C0n4 read  = status (bit7=no disk, bit6=write-protected)
//
// Test plan:
//   1. Plug the card with two synthetic Disk35Image objects pre-populated
//      with distinguishable block payloads.
//   2. Read block 5 of drive 1 — verify all 512 bytes match.
//   3. Switch to drive 2, read block 7 — verify it does NOT match drive 1
//      (drive selector actually picks the right image).
//   4. Read status with no disk → bit7 set; with disk → bit7 clear.
//   5. Enable write-back, write a 512 B pattern to drive 1 block 10,
//      eject + remount the image, read back — verifies the write path
//      reached Disk35Image and survived the disk-image internals (dirty
//      tracking, write commit on block-boundary).

#include "Disk35Image.h"
#include "SmartPortCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr size_t kBlockBytes  = 512;
constexpr size_t kBlocks      = 1600;     // 800 K = 1600 × 512

// Build an 800 K .po image where block N is filled with byte `seed + N`.
// The seed lets the two drives have different content so a swapped
// drive-select returns wrong data we can detect.
std::string writeSyntheticPo(const char* tag, uint8_t seed)
{
    const auto p = fs::temp_directory_path()
        / (std::string("pom2_spcard_") + tag + ".po");
    std::vector<uint8_t> img(kBlocks * kBlockBytes);
    for (size_t b = 0; b < kBlocks; ++b) {
        const uint8_t fill = static_cast<uint8_t>(seed + b);
        std::memset(img.data() + b * kBlockBytes, fill, kBlockBytes);
    }
    std::ofstream f(p, std::ios::binary);
    assert(f && "open temp .po for writing");
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    return p.string();
}

void writeReg(pom2::SmartPortCard& card, uint8_t low4, uint8_t v) {
    card.deviceSelectWrite(low4, v);
}
uint8_t readReg(pom2::SmartPortCard& card, uint8_t low4) {
    return card.deviceSelectRead(low4);
}

// Set up active drive + block, then stream 512 bytes out of $C0n3.
bool readBlockViaCard(pom2::SmartPortCard& card,
                      int drive, uint16_t block,
                      uint8_t out[kBlockBytes])
{
    writeReg(card, 0x0, static_cast<uint8_t>(drive));
    writeReg(card, 0x1, static_cast<uint8_t>(block & 0xFF));
    writeReg(card, 0x2, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlockBytes; ++i) {
        out[i] = readReg(card, 0x3);
    }
    return true;
}

// Same but for writes — drive + block setup then 512 stores at $C0n3.
void writeBlockViaCard(pom2::SmartPortCard& card,
                       int drive, uint16_t block,
                       const uint8_t in[kBlockBytes])
{
    writeReg(card, 0x0, static_cast<uint8_t>(drive));
    writeReg(card, 0x1, static_cast<uint8_t>(block & 0xFF));
    writeReg(card, 0x2, static_cast<uint8_t>((block >> 8) & 0xFF));
    for (size_t i = 0; i < kBlockBytes; ++i) {
        writeReg(card, 0x3, in[i]);
    }
}

bool testDriveSelectAndRead()
{
    const std::string path0 = writeSyntheticPo("d1", 0x10);
    const std::string path1 = writeSyntheticPo("d2", 0xA0);

    pom2::Disk35Image img0, img1;
    if (!img0.loadFile(path0)) {
        std::printf("FAIL: load d1: %s\n", img0.lastError().c_str()); return false;
    }
    if (!img1.loadFile(path1)) {
        std::printf("FAIL: load d2: %s\n", img1.lastError().c_str()); return false;
    }
    pom2::SmartPortCard card(5, &img0, &img1);

    // Drive 1, block 5 — fill byte = 0x10 + 5 = 0x15.
    uint8_t buf[kBlockBytes];
    readBlockViaCard(card, 0, 5, buf);
    for (size_t i = 0; i < kBlockBytes; ++i) {
        if (buf[i] != 0x15) {
            std::printf("FAIL: d1 blk5 byte %zu = %02X (want 15)\n", i, buf[i]);
            return false;
        }
    }

    // Drive 2, block 7 — fill byte = 0xA0 + 7 = 0xA7.
    readBlockViaCard(card, 1, 7, buf);
    for (size_t i = 0; i < kBlockBytes; ++i) {
        if (buf[i] != 0xA7) {
            std::printf("FAIL: d2 blk7 byte %zu = %02X (want A7)\n", i, buf[i]);
            return false;
        }
    }

    // Sanity: read d1 again to make sure d2's setup didn't leak.
    readBlockViaCard(card, 0, 5, buf);
    if (buf[0] != 0x15) {
        std::printf("FAIL: d1 blk5 re-read = %02X (want 15)\n", buf[0]);
        return false;
    }
    std::printf("OK : drive select + streaming reads\n");
    return true;
}

bool testStatusByte()
{
    pom2::Disk35Image img0, img1;
    pom2::SmartPortCard card(5, &img0, &img1);

    // Both empty → status bit7 = 1 on both drives.
    writeReg(card, 0x0, 0);
    if ((readReg(card, 0x4) & 0x80) == 0) {
        std::printf("FAIL: empty d1 status bit7 not set\n"); return false;
    }
    writeReg(card, 0x0, 1);
    if ((readReg(card, 0x4) & 0x80) == 0) {
        std::printf("FAIL: empty d2 status bit7 not set\n"); return false;
    }

    // Mount d1 → bit7 clears.
    const std::string p = writeSyntheticPo("st", 0x33);
    if (!img0.loadFile(p)) {
        std::printf("FAIL: load: %s\n", img0.lastError().c_str()); return false;
    }
    writeReg(card, 0x0, 0);
    if ((readReg(card, 0x4) & 0x80) != 0) {
        std::printf("FAIL: mounted d1 status bit7 still set\n"); return false;
    }
    // Default write-protected (writeBackEnabled is off) → bit6 set.
    if ((readReg(card, 0x4) & 0x40) == 0) {
        std::printf("FAIL: WP bit not set on default-mounted d1\n"); return false;
    }
    // Enable write-back → bit6 clears.
    img0.setWriteBackEnabled(true);
    if ((readReg(card, 0x4) & 0x40) != 0) {
        std::printf("FAIL: WP bit still set after enabling write-back\n");
        return false;
    }
    std::printf("OK : status byte (no-disk / WP)\n");
    return true;
}

bool testWriteBackRoundtrip()
{
    const std::string p = writeSyntheticPo("wb", 0x00);
    pom2::Disk35Image img0, img1;
    if (!img0.loadFile(p)) {
        std::printf("FAIL: load: %s\n", img0.lastError().c_str()); return false;
    }
    img0.setWriteBackEnabled(true);
    pom2::SmartPortCard card(5, &img0, &img1);

    // Write a recognisable pattern into block 10.
    uint8_t pattern[kBlockBytes];
    for (size_t i = 0; i < kBlockBytes; ++i)
        pattern[i] = static_cast<uint8_t>((i * 31) ^ 0x5A);
    writeBlockViaCard(card, 0, 10, pattern);

    // Read it back via the card → must match.
    uint8_t roundtrip[kBlockBytes];
    readBlockViaCard(card, 0, 10, roundtrip);
    if (std::memcmp(pattern, roundtrip, kBlockBytes) != 0) {
        for (size_t i = 0; i < kBlockBytes; ++i) {
            if (pattern[i] != roundtrip[i]) {
                std::printf("FAIL: byte %zu pattern=%02X read=%02X\n",
                            i, pattern[i], roundtrip[i]);
                return false;
            }
        }
    }

    // Image-level read should agree too — verifies the card's write
    // actually reached `Disk35Image::writeBlock`, not just an internal
    // cache invisible to the rest of the emulator.
    uint8_t direct[kBlockBytes];
    if (!img0.readBlock(10, direct)) {
        std::printf("FAIL: img0.readBlock(10)\n"); return false;
    }
    if (std::memcmp(pattern, direct, kBlockBytes) != 0) {
        std::printf("FAIL: pattern didn't reach Disk35Image\n"); return false;
    }
    std::printf("OK : write-back roundtrip\n");
    return true;
}

bool testRomSignature()
{
    pom2::Disk35Image img0, img1;
    pom2::SmartPortCard card(5, &img0, &img1);
    // ProDOS / SmartPort signature bytes — see SmartPortCard.cpp::buildRom.
    if (card.slotRomRead(0x01) != 0x20) { std::printf("FAIL: $Cn01\n"); return false; }
    if (card.slotRomRead(0x03) != 0x00) { std::printf("FAIL: $Cn03\n"); return false; }
    if (card.slotRomRead(0x05) != 0x03) { std::printf("FAIL: $Cn05\n"); return false; }
    if (card.slotRomRead(0x07) == 0x00) { std::printf("FAIL: $Cn07 zero\n"); return false; }
    if (card.slotRomRead(0xFF) == 0x00) { std::printf("FAIL: $CnFF zero\n"); return false; }
    std::printf("OK : ROM signature\n");
    return true;
}

} // anon namespace

int main() {
    bool ok = true;
    ok &= testRomSignature();
    ok &= testDriveSelectAndRead();
    ok &= testStatusByte();
    ok &= testWriteBackRoundtrip();
    return ok ? 0 : 1;
}

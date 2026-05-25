// Pins the MAME-faithful ATA taskfile core (AtaBlockDevice) — pure logic,
// no ROM required. Covers IDENTIFY capacity, single + multi-sector READ,
// WRITE round-trip, the sector-count==0 ⇒ 256 rule, DRQ/BSY status flow,
// and out-of-range LBA safety. P1 § Cartes de stockage MAME-fidèles.

#include "AtaBlockDevice.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using pom2::AtaBlockDevice;
using pom2::Block512Backing;

namespace {

constexpr size_t kBlk = Block512Backing::kBlockBytes;

// Deterministic per-block byte pattern.
uint8_t pat(uint32_t blk, size_t i) {
    return static_cast<uint8_t>((blk * 7u + i * 3u + 0x11u) & 0xFF);
}

std::vector<uint8_t> makeImage(uint32_t blocks) {
    std::vector<uint8_t> v(blocks * kBlk);
    for (uint32_t b = 0; b < blocks; ++b)
        for (size_t i = 0; i < kBlk; ++i)
            v[b * kBlk + i] = pat(b, i);
    return v;
}

void setLba(AtaBlockDevice& a, uint32_t lba, uint8_t count) {
    a.cs0_w(2, count);
    a.cs0_w(3, lba & 0xFF);
    a.cs0_w(4, (lba >> 8) & 0xFF);
    a.cs0_w(5, (lba >> 16) & 0xFF);
    a.cs0_w(6, 0xE0 | ((lba >> 24) & 0x0F)); // LBA mode, drive 0
}

// Pull one 512-byte sector out of the data port into dst.
void readSector(AtaBlockDevice& a, uint8_t* dst) {
    for (size_t i = 0; i < 256; ++i) {
        uint16_t w = a.cs0_r(0);
        dst[2 * i]     = static_cast<uint8_t>(w & 0xFF);
        dst[2 * i + 1] = static_cast<uint8_t>(w >> 8);
    }
}

void writeSector(AtaBlockDevice& a, const uint8_t* src) {
    for (size_t i = 0; i < 256; ++i) {
        uint16_t w = static_cast<uint16_t>(src[2 * i]) |
                     (static_cast<uint16_t>(src[2 * i + 1]) << 8);
        a.cs0_w(0, w);
    }
}

// Build a tiny write-protected 2IMG (flags bit0 = 1) on disk; returns its path.
std::string writeWp2img(uint32_t blocks) {
    const auto p = std::filesystem::temp_directory_path() / "pom2_ata_wp.2mg";
    std::vector<uint8_t> f(64 + blocks * kBlk, 0);
    std::memcpy(f.data(), "2IMG", 4);
    auto wr32 = [&](size_t o, uint32_t v) {
        f[o] = v & 0xFF; f[o + 1] = (v >> 8) & 0xFF;
        f[o + 2] = (v >> 16) & 0xFF; f[o + 3] = (v >> 24) & 0xFF;
    };
    wr32(12, 1);                       // format = ProDOS block order
    wr32(16, 1);                       // flags  = write-protected
    wr32(24, 64);                      // data offset
    wr32(28, blocks * kBlk);           // data length
    std::ofstream o(p, std::ios::binary);
    o.write(reinterpret_cast<const char*>(f.data()),
            static_cast<std::streamsize>(f.size()));
    return p.string();
}

} // namespace

int main() {
    // ── IDENTIFY reports the LBA28 capacity ───────────────────────────────
    {
        AtaBlockDevice a;
        const uint32_t blocks = 100;
        assert(a.backing().loadFromBytes(makeImage(blocks), "ata-test", ""));
        a.cs0_w(7, AtaBlockDevice::kCmdIdentify);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) != 0); // data ready
        uint16_t id[256];
        for (size_t i = 0; i < 256; ++i) id[i] = a.cs0_r(0);
        const uint32_t total =
            static_cast<uint32_t>(id[60]) | (static_cast<uint32_t>(id[61]) << 16);
        assert(total == blocks);
        // Words 57-58 (current capacity) must ALSO carry the count — the CFFA
        // firmware sizes its partitions from these, not 60-61. Zero here =>
        // "Could not boot partition 1 / Err $28".
        const uint32_t cur =
            static_cast<uint32_t>(id[57]) | (static_cast<uint32_t>(id[58]) << 16);
        assert(cur == blocks);
        assert(id[53] & 0x0001);                       // words 54-58 valid
        assert(id[49] & 0x0200);                       // LBA supported
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0); // transfer drained
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRDY);
    }

    // ── Single-sector READ matches the backing pattern ────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "r", ""));
        setLba(a, 3, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ);
        uint8_t buf[kBlk];
        readSector(a, buf);
        for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(3, i));
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);
    }

    // ── Multi-sector READ spans consecutive LBAs ──────────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "rm", ""));
        setLba(a, 6, 3);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        for (uint32_t s = 0; s < 3; ++s) {
            assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ);
            uint8_t buf[kBlk];
            readSector(a, buf);
            for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == pat(6 + s, i));
        }
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0); // done after 3
    }

    // ── WRITE round-trips through the backing ─────────────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(16), "w", ""));
        uint8_t src[kBlk];
        for (size_t i = 0; i < kBlk; ++i) src[i] = static_cast<uint8_t>(0xC0 ^ i);
        setLba(a, 5, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdWrite);
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ); // host may feed data
        writeSector(a, src);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);
        uint8_t back[kBlk];
        assert(a.backing().readBlock(5, back));
        for (size_t i = 0; i < kBlk; ++i) assert(back[i] == src[i]);
        // Re-read block 5 through the ATA path too.
        setLba(a, 5, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        uint8_t rd[kBlk];
        readSector(a, rd);
        for (size_t i = 0; i < kBlk; ++i) assert(rd[i] == src[i]);
        assert(a.backing().hasUnsavedChanges()); // block 5 marked dirty
    }

    // ── sectorCount == 0 means 256 sectors (DRQ persists past sector 1) ────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(257), "c0", ""));
        setLba(a, 0, 0); // count 0 ⇒ 256
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        uint8_t buf[kBlk];
        readSector(a, buf);                          // drain sector 1
        assert(a.cs0_r(7) & AtaBlockDevice::kStDRQ); // still requesting (≠ 1 sector)
    }

    // ── Out-of-range LBA reads as zeros, no crash ─────────────────────────
    {
        AtaBlockDevice a;
        assert(a.backing().loadFromBytes(makeImage(4), "oor", ""));
        setLba(a, 1000, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdRead);
        uint8_t buf[kBlk];
        readSector(a, buf);
        for (size_t i = 0; i < kBlk; ++i) assert(buf[i] == 0x00);
        assert((a.cs0_r(7) & AtaBlockDevice::kStDRQ) == 0);
    }

    // ── WRITE to a write-protected device aborts (ERR), no silent success ──
    {
        AtaBlockDevice a;
        const std::string p = writeWp2img(8);
        assert(a.backing().loadImage(p));
        assert(a.backing().isWriteProtected());
        setLba(a, 2, 1);
        a.cs0_w(7, AtaBlockDevice::kCmdWrite);
        const uint8_t st = static_cast<uint8_t>(a.cs0_r(7));
        assert((st & AtaBlockDevice::kStERR) != 0);   // error flagged in Status
        assert((st & AtaBlockDevice::kStDRQ) == 0);   // data phase NOT granted
        assert((static_cast<uint8_t>(a.cs0_r(1)) & AtaBlockDevice::kErrABRT) != 0);
        assert(!a.backing().hasUnsavedChanges());      // nothing written
        std::filesystem::remove(p);
    }

    std::printf("ata_block_device_test: OK\n");
    return 0;
}

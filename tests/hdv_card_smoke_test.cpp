// ProDOS HDV card smoke test — pins:
//  - ProDOS block-device signature bytes in $Cn00 ROM
//  - driver entry offset in $CnFF
//  - basic block read protocol via $C0D0-$C0D2 soft-switch window
//
// This is a core test: no ImGui, no OpenGL.

#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <vector>

static std::string writeTempHdv(const std::vector<uint8_t>& bytes)
{
    const std::string path = "/tmp/pom2_hdv_smoke.hdv";
    std::ofstream f(path, std::ios::binary);
    assert(f.good());
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    assert(f.good());
    return path;
}

int main()
{
    Memory mem;
    auto card = std::make_unique<ProDOSHardDiskCard>();

    // Build a tiny 2-block image with a known pattern in block 1.
    std::vector<uint8_t> hdv(2 * ProDOSHardDiskCard::kBlockBytes, 0x00);
    for (size_t i = 0; i < ProDOSHardDiskCard::kBlockBytes; ++i) {
        hdv[1 * ProDOSHardDiskCard::kBlockBytes + i] =
            static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
    }
    const std::string path = writeTempHdv(hdv);
    assert(card->loadImage(path));

    mem.slotBus().plug(ProDOSHardDiskCard::kSlot, std::move(card));

    // ProDOS signature bytes for block devices:
    // $Cn01=$20, $Cn03=$00, $Cn05=$03.
    assert(mem.memRead(0xC501) == 0x20);
    assert(mem.memRead(0xC503) == 0x00);
    assert(mem.memRead(0xC505) == 0x03);

    // $CnFF = driver entry offset, should point to $Cn50.
    assert(mem.memRead(0xC5FF) == 0x50);

    // Select block 1 then stream bytes via $C0D2.
    mem.memWrite(0xC0D0, 0x01); // block low
    mem.memWrite(0xC0D1, 0x00); // block high
    for (size_t i = 0; i < 16; ++i) {
        const uint8_t b = mem.memRead(0xC0D2);
        const uint8_t want = static_cast<uint8_t>((i * 7u + 3u) & 0xFFu);
        assert(b == want);
    }

    std::printf("HDV card smoke: OK\n");
    return 0;
}

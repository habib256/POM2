// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

// Disk II motor coast — the controller's 556 one-shot, on the LEGACY path.
//
// A motor-off ($C0E8) does not stop the spindle: the Disk II analog card
// keeps it driven for about one second, and DOS 3.3's RWTS RELIES on it —
// its "is the disk spinning?" check reads $C08C repeatedly BEFORE
// re-asserting $C0E9, and skips the one-second motor-on wait only if the
// latch moves. The bit-LSS path has modelled this since the MAME port
// (MODE_DELAY); the legacy nibble gate (plain .dsk with no P6 PROM — every
// headless test) stopped the drive instantly, so every RWTS call paid the
// full wait and a DOS 3.3 boot took ~115 s of machine time. What this pins:
//
//   1. While the motor is on, $C08C delivers a moving GCR stream.
//   2. After $C0E8 the drive still reports motor on and the latch keeps
//      moving through the ~1 s window (RWTS's check passes).
//   3. A $C0E9 inside the window cancels the countdown — still spinning
//      well past where the coast would have ended.
//   4. With no cancel, the drive stops after ~1 s: motor off, latch frozen
//      at $FF.

#include "DiskIICard.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>

namespace {

// Advance the card and sample the latch: how many distinct values, and did
// any carry bit 7 (a completed nibble)?
struct Sample { int distinct; bool sawByte; };
Sample sampleLatch(Memory& mem, DiskIICard& card, int reads, int gapCycles)
{
    std::set<uint8_t> seen;
    bool byte7 = false;
    for (int i = 0; i < reads; ++i) {
        card.advanceCycles(gapCycles);
        const uint8_t v = mem.memRead(0xC0EC);
        seen.insert(v);
        if (v & 0x80) byte7 = true;
    }
    return { static_cast<int>(seen.size()), byte7 };
}

} // namespace

int main()
{
    // A 140 K .dsk with VARIED sector data — an all-zero disk nibblizes
    // into long runs of one GCR nibble and a short latch sample can land
    // entirely inside one, which is movement the sampler cannot see.
    const std::filesystem::path dsk =
        std::filesystem::temp_directory_path() / "pom2_motor_coast.dsk";
    {
        std::ofstream f(dsk, std::ios::binary | std::ios::trunc);
        std::vector<char> data(143'360);
        for (size_t i = 0; i < data.size(); ++i)
            data[i] = static_cast<char>((i * 131) & 0xFF);
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
    }

    Memory mem;
    auto card = std::make_unique<DiskIICard>();
    if (!card->insertDisk(dsk.string())) {
        std::fprintf(stderr, "insertDisk failed: %s\n", card->getLastError().c_str());
        return 1;
    }
    DiskIICard* raw = card.get();
    mem.slotBus().plug(6, std::move(card));

    (void)mem.memRead(0xC0E9);            // motor on
    raw->advanceCycles(50'000);
    assert(raw->isMotorOn());

    // 1. Spinning: the latch moves and completes nibbles. 256 reads × 128
    //    cycles ≈ a thousand nibbles — several sectors of varied data.
    {
        const Sample s = sampleLatch(mem, *raw, 256, 128);
        assert(s.distinct >= 3 && s.sawByte && "motor on must deliver a moving stream");
    }

    // 2. Motor off: the 556 keeps the spindle driven — RWTS's pre-$C0E9
    //    spin check must still see movement through the window.
    (void)mem.memRead(0xC0E8);
    assert(raw->isMotorOn() && "the spindle coasts; $C0E8 is not an instant stop");
    for (int slice = 0; slice < 8; ++slice) {           // ~0.9 s in ~110 ms steps
        raw->advanceCycles(80'000);
        const Sample s = sampleLatch(mem, *raw, 256, 128);   // + ~33 k cycles
        assert(raw->isMotorOn());
        assert(s.distinct >= 3 && "the latch must keep moving while coasting");
    }

    // 3. $C0E9 inside the window cancels the countdown.
    (void)mem.memRead(0xC0E9);
    raw->advanceCycles(2'200'000);                      // well past any coast
    assert(raw->isMotorOn() && "a motor-on during the coast cancels the stop");
    {
        const Sample s = sampleLatch(mem, *raw, 256, 128);
        assert(s.distinct >= 3);
    }

    // 4. No cancel: the drive stops ~1 s after $C0E8 and the latch freezes.
    (void)mem.memRead(0xC0E8);
    raw->advanceCycles(1'100'000);
    assert(!raw->isMotorOn() && "the coast must end");
    {
        const Sample s = sampleLatch(mem, *raw, 16, 64);
        assert(s.distinct == 1 && "a stopped drive's latch is frozen");
        assert(mem.memRead(0xC0EC) == 0xFF);   // the frozen value is $FF
    }

    std::error_code ec;
    std::filesystem::remove(dsk, ec);
    std::printf("diskii motor coast: OK (spin, 1 s coast after $C0E8, $C0E9 cancel, stop)\n");
    return 0;
}

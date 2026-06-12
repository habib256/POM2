// ProDOSHardDiskCard slot-ROM driver — STATUS X/Y + no-media read test.
//
// Pins three driver-level contracts by executing the REAL slot ROM on a
// 6502 (same harness as smartport_write_dispatch_test):
//
//   1. STATUS (cmd $00) must return the device's total block count in
//      X (low) / Y (high) — ProDOS volume scanners (BITSY, ONLINE) size
//      the device from these registers, and a driver that left X/Y
//      unset returned garbage (the exact crash SmartPortCard::buildRom
//      already fixed; the HDV card now mirrors that ROM arrangement,
//      reading the count from $C0n4/$C0n5).
//
//   2. Counts above $FFFF clamp: an exactly-65536-block 32 MiB image
//      (Block512Backing::kMaxBlocks deliberately admits it) must report
//      $FFFF, not truncate to 0 ("empty volume").
//
//   3. READ (cmd $01) with no image mounted must return an I/O error —
//      carry set, A = $28 (NO DEVICE CONNECTED) — instead of streaming
//      $FF with CLC "success", per real ProDOS driver conventions.
//
// Plus a guard that a normal READ still works (the no-media probe grew
// the read routine by 9 bytes and moved the write-block branch target).

#include "M6502.h"
#include "Memory.h"
#include "ProDOSHardDiskCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

constexpr int    kSlot     = 5;
constexpr size_t kBlk      = ProDOSHardDiskCard::kBlockBytes;
constexpr uint8_t kRomHi   = 0xC0 + kSlot;          // $C5
constexpr uint16_t kDevBase = 0xC080 + kSlot * 16;  // $C0D0

// JSR $Cn50 with the given ProDOS parameter block, run to the park loop,
// return the CPU for register inspection.
void callDriver(M6502& cpu, Memory& mem, uint8_t cmd, uint16_t block,
                uint16_t buffer)
{
    mem.memWrite(0x42, cmd);
    mem.memWrite(0x43, static_cast<uint8_t>(kSlot << 4));  // unit
    mem.memWrite(0x44, static_cast<uint8_t>(buffer & 0xFF));
    mem.memWrite(0x45, static_cast<uint8_t>(buffer >> 8));
    mem.memWrite(0x46, static_cast<uint8_t>(block & 0xFF));
    mem.memWrite(0x47, static_cast<uint8_t>(block >> 8));

    mem.memWrite(0x0300, 0x20);              // JSR $Cn50
    mem.memWrite(0x0301, 0x50);
    mem.memWrite(0x0302, kRomHi);
    mem.memWrite(0x0303, 0x4C);              // JMP $0303 (park)
    mem.memWrite(0x0304, 0x03);
    mem.memWrite(0x0305, 0x03);

    cpu.setProgramCounter(0x0300);
    cpu.run(60000);                          // ample for a 512-byte loop
}

bool carrySet(const M6502& cpu) { return (cpu.getStatusRegister() & 0x01) != 0; }

}  // namespace

int main()
{
    Memory mem;
    M6502 cpu(&mem);
    cpu.hardReset();

    auto card = std::make_unique<ProDOSHardDiskCard>(kSlot);
    ProDOSHardDiskCard* raw = card.get();

    // 5-block image; block 1 carries a known pattern for the read guard.
    std::vector<uint8_t> hdv(5 * kBlk, 0x00);
    for (size_t i = 0; i < kBlk; ++i)
        hdv[kBlk + i] = static_cast<uint8_t>((i * 5u + 11u) & 0xFF);
    assert(raw->loadImageFromBytes(hdv, "status-test-5blk"));

    mem.slotBus().plug(kSlot, std::move(card));

    // ── 1. STATUS returns block count in X/Y ────────────────────────────
    callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
    if (carrySet(cpu) || cpu.getAccumulator() != 0x00) {
        std::printf("FAIL: STATUS returned error (A=%02X C=%d)\n",
                    cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
        return 1;
    }
    if (cpu.getXRegister() != 5 || cpu.getYRegister() != 0) {
        std::printf("FAIL: STATUS X/Y = %02X/%02X, want 05/00 "
                    "(block count not returned)\n",
                    cpu.getXRegister(), cpu.getYRegister());
        return 1;
    }
    // Register pair feeding the ROM routine.
    if (mem.memRead(kDevBase + 4) != 5 || mem.memRead(kDevBase + 5) != 0) {
        std::printf("FAIL: $C0n4/$C0n5 = %02X/%02X, want 05/00\n",
                    mem.memRead(kDevBase + 4), mem.memRead(kDevBase + 5));
        return 1;
    }

    // ── Guard: READ still works after the routine grew ──────────────────
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/1, /*buffer=*/0x0800);
    if (carrySet(cpu)) {
        std::printf("FAIL: READ of mounted block 1 returned carry set\n");
        return 1;
    }
    for (size_t i = 0; i < 16; ++i) {
        const uint8_t want = static_cast<uint8_t>((i * 5u + 11u) & 0xFF);
        if (mem.memRead(0x0800 + static_cast<uint16_t>(i)) != want) {
            std::printf("FAIL: READ data mismatch at +%zu: %02X want %02X\n",
                        i, mem.memRead(0x0800 + static_cast<uint16_t>(i)),
                        want);
            return 1;
        }
    }

    // ── 2. 65536-block (32 MiB) volume clamps to $FFFF ──────────────────
    {
        std::vector<uint8_t> big(65536ull * kBlk, 0x00);
        assert(raw->loadImageFromBytes(std::move(big), "status-test-32MiB"));
        assert(raw->getBlockCount() == 65536u);
        callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
        if (carrySet(cpu) ||
            cpu.getXRegister() != 0xFF || cpu.getYRegister() != 0xFF) {
            std::printf("FAIL: 65536-block STATUS X/Y = %02X/%02X, want "
                        "FF/FF (truncated to 0?)\n",
                        cpu.getXRegister(), cpu.getYRegister());
            return 1;
        }
    }

    // ── 3. READ with no media → carry set, A = $28 ──────────────────────
    raw->ejectImage();
    assert(!raw->isImageLoaded());
    mem.memWrite(0x0800, 0xA7);              // sentinel: must stay intact
    callDriver(cpu, mem, /*cmd=*/0x01, /*block=*/0, /*buffer=*/0x0800);
    if (!carrySet(cpu) || cpu.getAccumulator() != 0x28) {
        std::printf("FAIL: no-media READ returned A=%02X C=%d, want "
                    "A=28 C=1 (was CLC \"success\" over a $FF stream)\n",
                    cpu.getAccumulator(), carrySet(cpu) ? 1 : 0);
        return 1;
    }
    if (mem.memRead(0x0800) != 0xA7) {
        std::printf("FAIL: no-media READ clobbered the caller buffer "
                    "($0800 = %02X)\n", mem.memRead(0x0800));
        return 1;
    }
    // STATUS on the empty bay reports 0 blocks (and the count registers
    // read 0, not $FF garbage).
    callDriver(cpu, mem, /*cmd=*/0x00, /*block=*/0, /*buffer=*/0x0800);
    if (cpu.getXRegister() != 0 || cpu.getYRegister() != 0) {
        std::printf("FAIL: empty-bay STATUS X/Y = %02X/%02X, want 00/00\n",
                    cpu.getXRegister(), cpu.getYRegister());
        return 1;
    }

    std::printf("OK hdv_status_driver (STATUS X/Y + clamp + no-media $28)\n");
    return 0;
}

// Snapshot full-state round-trip regression test.
//
// Two bugs this pins (round-2 audit):
//   * The snapshot CPU section recorded A/X/Y/P/SP but load() discarded
//     them — M6502 only had setProgramCounter. New register setters let the
//     restore reconstruct the full CPU state.
//   * The snapshot MEM section captured only the visible main 64 KB; aux
//     RAM, Language-Card RAM, RamWorks banks, the IIe paging soft-switches,
//     and DisplayState were never serialized, so any IIe/aux/LC program
//     restored garbage. Memory::appendSnapshotState / loadSnapshotState now
//     round-trip that extended state.
//
// Drives M6502 + Memory directly (no AiControlServer / sockets); the server
// just calls these same setters / (de)serializers.

#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// Page in Language-Card bank-1 RAM, read+write enabled (two consecutive
// odd $C08B accesses arm the sticky write-enable).
void pageLcBank1RW(Memory& m) {
    (void)m.memRead(0xC08B);
    (void)m.memRead(0xC08B);
}

}  // namespace

int main()
{
    // ── Part A: M6502 register setters (CPU section restore) ─────────────
    {
        Memory mem; mem.setTestMode(true);
        M6502 cpu(&mem);
        cpu.setProgramCounter(0x1234);
        cpu.setAccumulator(0xA5);
        cpu.setXRegister(0x11);
        cpu.setYRegister(0x22);
        cpu.setStatusRegister(0xC3);
        cpu.setStackPointer(0xF0);
        cpu.setCpuMode(M6502::CpuMode::CMOS);
        CHECK(cpu.getProgramCounter() == 0x1234, "PC setter");
        CHECK(cpu.getAccumulator()    == 0xA5,   "A setter");
        CHECK(cpu.getXRegister()      == 0x11,   "X setter");
        CHECK(cpu.getYRegister()      == 0x22,   "Y setter");
        CHECK(cpu.getStatusRegister() == 0xC3,   "P setter");
        CHECK(cpu.getStackPointer()   == 0xF0,   "SP setter");
    }

    // ── Part B: Memory extended-state blob round-trip ────────────────────
    {
        Memory src; src.setIIEMode(true);
        // Paging soft-switches (write-only on //e): 80STORE + RAMRD + RAMWRT.
        src.memWrite(0xC001, 0);   // 80STORE on
        src.memWrite(0xC003, 0);   // RAMRD on
        src.memWrite(0xC005, 0);   // RAMWRT on
        // DisplayState via display switches: HIRES on, PAGE2 on.
        src.memWrite(0xC057, 0);   // HIRES on
        src.memWrite(0xC055, 0);   // PAGE2 on
        // Aux RAM sentinels (written straight to the aux array).
        uint8_t* sa = src.auxDataMutable();
        sa[0x2000] = 0xBE; sa[0x5000] = 0xEF; sa[0xBFFF] = 0x42;
        // Cycle counter.
        src.setCycleCounter(0x00DEADBEEFull);
        // Language-Card RAM (bank 1 + shared high) via the paging path.
        pageLcBank1RW(src);
        src.memWrite(0xD000, 0x77);   // → lcBank1[0]
        src.memWrite(0xE000, 0x88);   // → lcHigh[0]

        const uint16_t srcMode = src.iieModeFlags();
        const auto     srcDisp = src.getDisplayState();

        std::vector<uint8_t> blob;
        src.appendSnapshotState(blob);
        CHECK(!blob.empty(), "blob produced");

        // Fresh target in a different state.
        Memory dst; dst.setIIEMode(true);
        const bool ok = dst.loadSnapshotState(blob.data(), blob.size());
        CHECK(ok, "loadSnapshotState ok");

        CHECK(dst.iieModeFlags() == srcMode, "iieMemMode round-trip");
        const auto d = dst.getDisplayState();
        CHECK(d.hiRes == srcDisp.hiRes && d.hiRes,           "DisplayState hiRes");
        CHECK(d.page2 == srcDisp.page2 && d.page2,           "DisplayState page2");
        CHECK(d.eightyStore == srcDisp.eightyStore,          "DisplayState 80store");
        CHECK(dst.getCycleCounter() == 0x00DEADBEEFull,      "cycleCounter round-trip");

        const uint8_t* da = dst.auxData();
        CHECK(da[0x2000] == 0xBE, "aux $2000 round-trip");
        CHECK(da[0x5000] == 0xEF, "aux $5000 round-trip");
        CHECK(da[0xBFFF] == 0x42, "aux $BFFF round-trip");

        // LC RAM: read back through the same paging path.
        pageLcBank1RW(dst);
        CHECK(dst.memRead(0xD000) == 0x77, "LC bank1 $D000 round-trip");
        CHECK(dst.memRead(0xE000) == 0x88, "LC high  $E000 round-trip");
    }

    // ── Part C: restoreMainRam restores RAM cells ────────────────────────
    {
        Memory src;
        src.writeRamUnchecked(0x1000, 0xAB);
        src.writeRamUnchecked(0xBFFF, 0xCD);
        std::vector<uint8_t> snap(0x10000);
        for (size_t i = 0; i < 0x10000; ++i) snap[i] = src.data()[i];

        Memory dst;
        dst.restoreMainRam(snap.data(), snap.size());
        CHECK(dst.data()[0x1000] == 0xAB, "restoreMainRam $1000");
        CHECK(dst.data()[0xBFFF] == 0xCD, "restoreMainRam $BFFF");
    }

    if (failures == 0) std::printf("OK snapshot_state_roundtrip\n");
    return failures == 0 ? 0 : 1;
}

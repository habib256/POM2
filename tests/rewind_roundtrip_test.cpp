// Rewind ring-buffer round-trip test (Phase 1).
//
// Drives Memory + M6502 directly (no ImGui / controller) and pins the core
// rewind contract:
//   1. capture → restore reproduces CPU registers + main RAM + cycle
//      counter bit-for-bit;
//   2. the ring evicts oldest-first once over its frame cap;
//   3. restoreToCycle() lands on the newest frame at-or-before a target
//      cycle (and clamps to the oldest when the target predates the ring);
//   4. a disabled buffer captures nothing (zero overhead).
//
// This is the gate for any change to RewindBuffer or the MachineSnapshot
// capture/restore sequence it rides on.

#include "M6502.h"
#include "Memory.h"
#include "RewindBuffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

// Stamp a recognisable, fully-recoverable machine state keyed by `tag`.
// Uses main-RAM addresses ($0300/$6000) that are plain writable RAM on the
// default (II+) profile, so restoreMainRam round-trips them.
void setState(M6502& cpu, Memory& mem, uint8_t tag, uint64_t cyc)
{
    cpu.setAccumulator(tag);
    cpu.setXRegister(static_cast<uint8_t>(tag ^ 0xFF));
    cpu.setYRegister(static_cast<uint8_t>(tag + 0x10));
    cpu.setStackPointer(static_cast<uint8_t>(0xF0 - tag));
    cpu.setStatusRegister(static_cast<uint8_t>((tag & 0x0F) | 0x20));
    cpu.setProgramCounter(static_cast<uint16_t>(0x1000 + tag));
    mem.setCycleCounter(cyc);
    mem.memWrite(0x0300, tag);
    mem.memWrite(0x6000, static_cast<uint8_t>(tag * 3 + 1));
    mem.memWrite(0x9000, static_cast<uint8_t>(0xA0 ^ tag));
}

void checkState(M6502& cpu, Memory& mem, uint8_t tag, uint64_t cyc)
{
    assert(cpu.getAccumulator()    == tag);
    assert(cpu.getXRegister()      == static_cast<uint8_t>(tag ^ 0xFF));
    assert(cpu.getYRegister()      == static_cast<uint8_t>(tag + 0x10));
    assert(cpu.getStackPointer()   == static_cast<uint8_t>(0xF0 - tag));
    assert(cpu.getStatusRegister() == static_cast<uint8_t>((tag & 0x0F) | 0x20));
    assert(cpu.getProgramCounter() == static_cast<uint16_t>(0x1000 + tag));
    assert(mem.getCycleCounter()   == cyc);
    assert(mem.data()[0x0300] == tag);
    assert(mem.data()[0x6000] == static_cast<uint8_t>(tag * 3 + 1));
    assert(mem.data()[0x9000] == static_cast<uint8_t>(0xA0 ^ tag));
}

void scramble(M6502& cpu, Memory& mem)
{
    setState(cpu, mem, 0x5A, 0xDEADBEEF);   // anything distinct from a tag
}

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);

    // ── (4) Disabled buffer captures nothing ──────────────────────────────
    {
        pom2::RewindBuffer rb;
        assert(!rb.enabled());
        setState(cpu, mem, 1, 1000);
        rb.capture(cpu, mem);
        assert(rb.empty());
    }

    // ── (1) Bit-for-bit capture → restore ─────────────────────────────────
    {
        pom2::RewindBuffer rb;
        rb.setEnabled(true);

        setState(cpu, mem, 11, 11'000);
        rb.capture(cpu, mem);                 // frame 0
        setState(cpu, mem, 22, 22'000);
        rb.capture(cpu, mem);                 // frame 1
        setState(cpu, mem, 33, 33'000);
        rb.capture(cpu, mem);                 // frame 2
        assert(rb.size() == 3);
        assert(rb.oldestCycle() == 11'000 && rb.newestCycle() == 33'000);

        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, 11, 11'000);

        scramble(cpu, mem);
        assert(rb.restore(2, cpu, mem));
        checkState(cpu, mem, 33, 33'000);

        scramble(cpu, mem);
        assert(rb.restore(1, cpu, mem));
        checkState(cpu, mem, 22, 22'000);

        assert(!rb.restore(3, cpu, mem));     // out of range
    }

    // ── (3) restoreToCycle lands on newest frame <= target ────────────────
    {
        pom2::RewindBuffer rb;
        rb.setEnabled(true);
        setState(cpu, mem, 1, 1000); rb.capture(cpu, mem);
        setState(cpu, mem, 2, 2000); rb.capture(cpu, mem);
        setState(cpu, mem, 3, 3000); rb.capture(cpu, mem);

        scramble(cpu, mem);
        assert(rb.restoreToCycle(2500, cpu, mem) == 1);   // newest <= 2500 → 2000
        checkState(cpu, mem, 2, 2000);

        scramble(cpu, mem);
        assert(rb.restoreToCycle(3000, cpu, mem) == 2);   // exact match
        checkState(cpu, mem, 3, 3000);

        scramble(cpu, mem);
        assert(rb.restoreToCycle(50, cpu, mem) == 0);     // predates ring → oldest
        checkState(cpu, mem, 1, 1000);

        pom2::RewindBuffer empty;
        assert(empty.restoreToCycle(123, cpu, mem) == pom2::RewindBuffer::kNoFrame);
    }

    // ── (2) Ring eviction: oldest-first, cap honoured ─────────────────────
    {
        pom2::RewindBuffer rb(3);             // cap = 3 frames
        rb.setEnabled(true);
        for (uint8_t i = 0; i < 5; ++i) {
            setState(cpu, mem, static_cast<uint8_t>(40 + i),
                     static_cast<uint64_t>(40 + i) * 1000);
            rb.capture(cpu, mem);
        }
        assert(rb.size() == 3);
        // Frames 0,1 evicted; the survivors are tags 42,43,44 (cycles 42k..44k).
        assert(rb.oldestCycle() == 42'000);
        assert(rb.newestCycle() == 44'000);
        assert(rb.infoAt(0).cycle == 42'000);
        assert(rb.infoAt(0).bytes > 0);

        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, 42, 42'000);

        // Shrinking the cap evicts immediately.
        rb.setMaxFrames(1);
        assert(rb.size() == 1);
        assert(rb.newestCycle() == 44'000);   // the most recent survives

        rb.clear();
        assert(rb.empty() && rb.bytes() == 0);
    }

    std::printf("Rewind ring buffer: OK (round-trip + eviction + seek)\n");
    return 0;
}

// Rewind delta-codec + keyframe test (Phase 2).
//
// RewindBuffer keeps memory bounded by storing periodic full keyframes and
// XOR deltas in between. This test forces a small keyframe interval so
// reconstruction crosses several keyframe boundaries, then pins:
//   1. EVERY stored frame restores bit-for-bit (CPU + RAM + cycle), proving
//      the keyframe/delta reconstruction is exact;
//   2. deltas actually shrink memory vs all-keyframes, and keyframes land at
//      the configured cadence;
//   3. eviction (with keyframe rebasing) keeps every survivor restorable;
//   4. truncateAfter() drops the future and lets capture continue exactly.
//
// The Phase 1 test (rewind_roundtrip) already pins the public API against the
// full-snapshot behaviour; this one pins the compressed path behind it.

#include "M6502.h"
#include "Memory.h"
#include "RewindBuffer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

void setState(M6502& cpu, Memory& mem, uint8_t tag, uint64_t cyc)
{
    cpu.setAccumulator(tag);
    cpu.setXRegister(static_cast<uint8_t>(tag ^ 0xFF));
    cpu.setYRegister(static_cast<uint8_t>(tag + 0x10));
    cpu.setStackPointer(static_cast<uint8_t>(0xF0 - tag));
    cpu.setStatusRegister(static_cast<uint8_t>((tag & 0x0F) | 0x20));
    cpu.setProgramCounter(static_cast<uint16_t>(0x1000 + tag));
    mem.setCycleCounter(cyc);
    // A few scattered RAM writes — exercises the delta span coalescing.
    mem.memWrite(0x0300, tag);
    mem.memWrite(0x0400, static_cast<uint8_t>(tag + 7));
    mem.memWrite(0x2000, static_cast<uint8_t>(tag * 5));
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
    assert(mem.data()[0x0400] == static_cast<uint8_t>(tag + 7));
    assert(mem.data()[0x2000] == static_cast<uint8_t>(tag * 5));
    assert(mem.data()[0x6000] == static_cast<uint8_t>(tag * 3 + 1));
    assert(mem.data()[0x9000] == static_cast<uint8_t>(0xA0 ^ tag));
}

void scramble(M6502& cpu, Memory& mem) { setState(cpu, mem, 0x5A, 0xDEADBEEF); }

// tag/cycle for the k-th captured frame.
uint8_t  tagOf(int k)   { return static_cast<uint8_t>(1 + k); }
uint64_t cycOf(int k)   { return static_cast<uint64_t>(1 + k) * 1000; }

}  // namespace

int main()
{
    Memory mem;
    M6502  cpu(&mem);
    cpu.setCpuMode(M6502::CpuMode::NMOS);

    // ── (1) + (2) Bit-exact across keyframe boundaries; memory shrinks ─────
    {
        pom2::RewindBuffer rb(1000);
        rb.setEnabled(true);
        rb.setKeyframeInterval(5);            // keyframe every 5 frames

        const int N = 23;
        for (int k = 0; k < N; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        assert(rb.size() == static_cast<size_t>(N));

        // Keyframes at indices 0,5,10,15,20 → ceil(23/5) = 5.
        assert(rb.keyframeCount() == 5);
        assert(rb.infoAt(0).keyframe);
        assert(rb.infoAt(5).keyframe);
        assert(!rb.infoAt(1).keyframe);
        assert(!rb.infoAt(6).keyframe);

        // Every single frame must round-trip exactly — including the frames
        // farthest from their keyframe (4, 9, 14, ...).
        for (int k = N - 1; k >= 0; --k) {     // reverse order = real rewind
            scramble(cpu, mem);
            assert(rb.restore(static_cast<size_t>(k), cpu, mem));
            checkState(cpu, mem, tagOf(k), cycOf(k));
        }

        // Deltas must be much smaller than storing N full keyframes. A full
        // blob is the keyframe size; deltas here touch only regs + ~5 bytes.
        const size_t keyframeBytes = rb.infoAt(0).bytes;
        const size_t deltaBytes    = rb.infoAt(1).bytes;
        assert(deltaBytes * 4 < keyframeBytes);             // deltas are tiny
        assert(rb.bytes() < keyframeBytes * static_cast<size_t>(N) / 2);  // big win

        // restoreToCycle still lands on the right frame through the codec.
        scramble(cpu, mem);
        assert(rb.restoreToCycle(cycOf(12) + 500, cpu, mem) == 12);
        checkState(cpu, mem, tagOf(12), cycOf(12));
    }

    // ── (3) Eviction with keyframe rebasing keeps survivors restorable ─────
    {
        pom2::RewindBuffer rb(7);             // small ring
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);

        const int N = 30;
        for (int k = 0; k < N; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        assert(rb.size() == 7);
        // Front must be a keyframe (the rebase invariant) so reconstruction
        // of every survivor is well-defined.
        assert(rb.infoAt(0).keyframe);

        const uint64_t oldest = rb.oldestCycle();
        const uint64_t newest = rb.newestCycle();
        assert(newest == cycOf(N - 1));
        for (size_t i = 0; i < rb.size(); ++i) {
            const uint64_t c = rb.infoAt(i).cycle;
            assert(c >= oldest && c <= newest);
            // map cycle back to its tag: cyc = (tag) * 1000, tag = 1 + k.
            const uint8_t tag = static_cast<uint8_t>(c / 1000);
            scramble(cpu, mem);
            assert(rb.restore(i, cpu, mem));
            checkState(cpu, mem, tag, c);
        }
    }

    // ── (4) truncateAfter() drops the future; capture continues exactly ────
    {
        pom2::RewindBuffer rb(1000);
        rb.setEnabled(true);
        rb.setKeyframeInterval(4);

        for (int k = 0; k < 12; ++k) {
            setState(cpu, mem, tagOf(k), cycOf(k));
            rb.capture(cpu, mem);
        }
        rb.truncateAfter(5);                  // keep frames 0..5
        assert(rb.size() == 6);
        assert(rb.newestCycle() == cycOf(5));

        // Old frames still restore.
        scramble(cpu, mem);
        assert(rb.restore(5, cpu, mem));
        checkState(cpu, mem, tagOf(5), cycOf(5));

        // Resume capturing on the new timeline; the first appended frame
        // delta-bases off frame 5 (truncate rebuilt prevBlob_).
        setState(cpu, mem, 200, 999'000);
        rb.capture(cpu, mem);
        assert(rb.size() == 7);
        scramble(cpu, mem);
        assert(rb.restore(6, cpu, mem));
        checkState(cpu, mem, 200, 999'000);
        // …and the pre-truncate frames are still intact.
        scramble(cpu, mem);
        assert(rb.restore(0, cpu, mem));
        checkState(cpu, mem, tagOf(0), cycOf(0));
    }

    std::printf("Rewind delta codec: OK (keyframe/delta exact + evict + truncate)\n");
    return 0;
}

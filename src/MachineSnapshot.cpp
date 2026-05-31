// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MachineSnapshot — see MachineSnapshot.h. Extracted verbatim from the
// AiControlServer `/snapshot/save|load` handlers so the rewind ring buffer
// and the HTTP API serialize the exact same bytes.

#include "MachineSnapshot.h"

#include "M6502.h"
#include "Memory.h"
#include "SlotBus.h"
#include "SlotPeripheral.h"
#include "SnapshotIO.h"

#include <cstdint>
#include <vector>

namespace pom2 {

void captureMachineState(SnapshotWriter& w, M6502& cpu, Memory& mem,
                         bool includeSlots)
{
    // CPU: PC(2) A X Y P SP cpuMode (6) + absolute cycle counter (8) = 16 B.
    // IRQ/NMI lines are transient and self-correct within a frame, so they
    // are not persisted (see SnapshotIO.h).
    {
        SnapshotWriter::SectionHandle h = w.beginSection("CPU");
        w.writeU16(cpu.getProgramCounter());
        w.writeU8 (cpu.getAccumulator());
        w.writeU8 (cpu.getXRegister());
        w.writeU8 (cpu.getYRegister());
        w.writeU8 (cpu.getStatusRegister());
        w.writeU8 (cpu.getStackPointer());
        w.writeU8 (cpu.getCpuMode() == M6502::CpuMode::CMOS ? 1 : 0);
        w.writeU64(mem.getCycleCounter());
        w.endSection(h);
    }
    w.writeSection("MEM", mem.data(), 0x10000);
    // MEX (v2): aux RAM + Language-Card RAM + RamWorks banks + paging soft-
    // switches + DisplayState — everything the MEM main-64K misses.
    {
        std::vector<uint8_t> mex;
        mem.appendSnapshotState(mex);
        w.writeSection("MEX", mex.data(), mex.size());
    }
    // SLOT1..SLOT7: per-card volatile runtime state (e.g. DiskIICard's head
    // position + LSS). Opt-in: only the rewind path wants these (the
    // AI-control file snapshot keeps disk/slot state excluded — see header).
    // Cards with nothing rewindable append nothing → no section.
    if (includeSlots) {
        SlotBus& bus = mem.slotBus();
        for (int slot = 1; slot <= 7; ++slot) {
            SlotPeripheral* card = bus.peripheral(slot);
            if (!card) continue;
            std::vector<uint8_t> blob;
            card->appendSnapshotState(blob);
            if (blob.empty()) continue;
            const char name[6] = { 'S', 'L', 'O', 'T',
                                   static_cast<char>('0' + slot), '\0' };
            w.writeSection(name, blob.data(), blob.size());
        }
    }
}

namespace {
// "SLOTn" → n in 1..7, else 0 (not a per-slot section).
int parseSlotSection(const std::string& name)
{
    if (name.size() == 5 && name.compare(0, 4, "SLOT") == 0 &&
        name[4] >= '1' && name[4] <= '7')
        return name[4] - '0';
    return 0;
}
}  // namespace

RestoreResult restoreMachineState(SnapshotReader& r, M6502& cpu, Memory& mem)
{
    std::string name;
    uint32_t len = 0;
    while (r.nextSection(name, len)) {
        // Require the FULL 16-byte CPU section. The block below consumes 16
        // bytes unconditionally; a gate of `>= 9` let a crafted/truncated
        // section (9..15 B) read up to 7 bytes past it → garbage cycle
        // counter / CPU mode. A normal save always writes exactly 16.
        if (name == "CPU" && len >= 16) {
            const uint16_t pc      = r.readU16();
            const uint8_t  a       = r.readU8();
            const uint8_t  x       = r.readU8();
            const uint8_t  y       = r.readU8();
            const uint8_t  p       = r.readU8();
            const uint8_t  sp      = r.readU8();
            const uint8_t  cpuMode = r.readU8();
            const uint64_t cycles  = r.readU64();
            cpu.setProgramCounter(pc);
            cpu.setCpuMode(cpuMode ? M6502::CpuMode::CMOS : M6502::CpuMode::NMOS);
            cpu.setAccumulator(a);
            cpu.setXRegister(x);
            cpu.setYRegister(y);
            cpu.setStatusRegister(p);
            cpu.setStackPointer(sp);
            mem.setCycleCounter(cycles);
        } else if (name == "MEM" && len == 0x10000) {
            // Restore the main 64 KB through writable[] so the ROM mirror in
            // $C000-$FFFF isn't clobbered (LC RAM is restored via MEX).
            std::vector<uint8_t> buf(0x10000);
            r.readBytes(buf.data(), buf.size());
            mem.restoreMainRam(buf.data(), buf.size());
        } else if (name == "MEX") {
            // Bound the allocation. nextSection() already rejects len > blob
            // size; cap here too (a legit MEX is ≤ ~11 MB: aux + LC + 128
            // RamWorks banks) so even a large crafted file can't OOM us.
            constexpr uint32_t kMaxMexBytes = 16u * 1024u * 1024u;
            if (len > kMaxMexBytes) {
                return { false, "snapshot MEX section too large" };
            }
            std::vector<uint8_t> buf(len);
            if (len) r.readBytes(buf.data(), len);
            mem.loadSnapshotState(buf.data(), len);
        } else if (const int slot = parseSlotSection(name)) {
            // Per-card state. Bound the alloc (nextSection already rejects
            // len > blob size; cap again so a crafted file can't OOM us — a
            // real card blob is well under this).
            constexpr uint32_t kMaxSlotBytes = 1u * 1024u * 1024u;
            if (len > kMaxSlotBytes) { r.skipCurrentSection(); continue; }
            std::vector<uint8_t> buf(len);
            if (len) r.readBytes(buf.data(), len);
            // Apply only if a card sits there now; a card type mismatch is
            // handled by each card ignoring foreign (magic-tagged) blobs.
            if (SlotPeripheral* card = mem.slotBus().peripheral(slot))
                card->loadSnapshotState(buf.data(), len);
        } else {
            r.skipCurrentSection();
        }
    }
    return {};
}

}  // namespace pom2

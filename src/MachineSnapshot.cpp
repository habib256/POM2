// VERHILLE Arnaud 2026

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
    // CPU: PC(2) A X Y P SP cpuMode (6) + absolute cycle counter (8) +
    // STP halt latch (1) = 17 B. IRQ/NMI lines are transient and
    // self-correct within a frame, so they are not persisted (see
    // SnapshotIO.h); `halted` is NOT transient — only RESET clears it.
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
        w.writeU8 (cpu.isHalted() ? 1 : 0);
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

namespace {

RestoreResult applyMachineState(SnapshotReader& r, M6502& cpu, Memory& mem)
{
    // Disarm any live DMA bus master (SoftCard Z80) BEFORE restoring.
    // File snapshots are captured with includeSlots=false, so the incoming
    // blob usually has no SLOTn section to overwrite a claimant's state —
    // without this, loading a snapshot while CP/M runs left the Z80
    // enabled and executing over the freshly restored RAM at a stale PC,
    // and the restored 6502 never ran (2026-07-12 bug hunt). onReset is
    // the bus-accurate verb (MAME reset_from_bus). Snapshots that DO
    // carry a SLOTn blob (rewind ring) re-arm the card right below.
    // LAZY: run this only once we are about to apply a section that
    // actually mutates the machine. A file that turns out to be empty,
    // foreign or truncated-at-the-first-header used to kick a live bus
    // master off the bus before anything was even read.
    bool dmaDisarmed = false;
    auto disarmDmaOnce = [&]() {
        if (dmaDisarmed) return;
        dmaDisarmed = true;
        for (int s = 0; s < SlotBus::kSlotCount; ++s) {
            SlotPeripheral* card = mem.slotBus().peripheral(s);
            if (card && card->dmaActive())
                card->onReset();
        }
    };

    std::string name;
    uint32_t len = 0;
    bool appliedCore = false;   // CPU or MEM actually restored
    while (r.nextSection(name, len)) {
        // Require the FULL 16-byte CPU section. The block below consumes 16
        // bytes unconditionally; a gate of `>= 9` let a crafted/truncated
        // section (9..15 B) read up to 7 bytes past it → garbage cycle
        // counter / CPU mode. A normal save always writes exactly 16.
        if (name == "CPU" && len >= 16) {
            disarmDmaOnce();
            appliedCore = true;
            const uint16_t pc      = r.readU16();
            const uint8_t  a       = r.readU8();
            const uint8_t  x       = r.readU8();
            const uint8_t  y       = r.readU8();
            const uint8_t  p       = r.readU8();
            const uint8_t  sp      = r.readU8();
            const uint8_t  cpuMode = r.readU8();
            const uint64_t cycles  = r.readU64();
            // v1.1 tail: STP halt latch. Legacy 16-byte blobs predate the
            // halted capture and were (almost) always taken while running
            // — clearing is the correct default AND fixes the common
            // rewind-out-of-a-crash case, where the live `halted` used to
            // survive the restore and keep the machine frozen.
            const bool halted = (len >= 17) ? (r.readU8() != 0) : false;
            cpu.setProgramCounter(pc);
            // cpuMode is read to keep the section cursor math intact but
            // NOT applied: CPU mode is machine CONFIGURATION (profile +
            // cpu_mode_override, with resolveCpuMode's soldered-65C02
            // clamp on //c-class), not machine state. Applying a foreign
            // snapshot's byte bypassed that clamp — an NMOS-mode blob
            // loaded on a //c forced its 65C02 ROM onto an NMOS core (KIL
            // freeze), and the override persisted across resets. Same
            // precedent as MEX's iieMode field (Memory.cpp).
            (void)cpuMode;
            cpu.setAccumulator(a);
            cpu.setXRegister(x);
            cpu.setYRegister(y);
            cpu.setStatusRegister(p);
            cpu.setStackPointer(sp);
            cpu.setHalted(halted);
            mem.setCycleCounter(cycles);
        } else if (name == "MEM" && len == 0x10000) {
            // Restore the main 64 KB through writable[] so the ROM mirror in
            // $C000-$FFFF isn't clobbered (LC RAM is restored via MEX).
            disarmDmaOnce();
            appliedCore = true;
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
            disarmDmaOnce();
            std::vector<uint8_t> buf(len);
            if (len) r.readBytes(buf.data(), len);
            // Surface a malformed MEX honestly. The public wrapper restores
            // its pre-load checkpoint if any section fails after mutation.
            if (!mem.loadSnapshotState(buf.data(), len)) {
                return { false, "snapshot MEX section truncated or malformed" };
            }
        } else if (const int slot = parseSlotSection(name)) {
            // Per-card state. Bound the alloc (nextSection already rejects
            // len > blob size; cap again so a crafted file can't OOM us — a
            // real card blob is well under this).
            constexpr uint32_t kMaxSlotBytes = 1u * 1024u * 1024u;
            if (len > kMaxSlotBytes) { r.skipCurrentSection(); continue; }
            disarmDmaOnce();
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
    // The section loop exits on BOTH clean EOF and mid-file truncation
    // (nextSection returns false either way). Distinguish them: a section
    // whose declared length runs past EOF sets ok=false + errorMsg, and a
    // stream failbit means a torn header — both left the machine
    // HALF-restored while this function reported success. good() is
    // `ok && !fail()`, so a normal EOF (eofbit only) still passes.
    if (!r.good()) {
        return { false,
                 r.error().empty() ? "snapshot truncated or corrupt"
                                   : r.error() };
    }
    if (!appliedCore) {
        // Well-formed but carrying neither CPU nor MEM: nothing was
        // restored, so reporting success would leave the caller (and the
        // user) believing a load happened.
        return { false, "snapshot contains no restorable CPU/MEM sections" };
    }
    return {};
}

}  // namespace

RestoreResult restoreMachineState(SnapshotReader& r, M6502& cpu, Memory& mem,
                                  bool transactional)
{
    if (!transactional)
        return applyMachineState(r, cpu, mem);

    // A file/API snapshot is untrusted and sections are applied incrementally.
    // Capture every restorable surface first, including slot/DMA state, so a
    // late malformed MEX or torn trailing section cannot leave a hybrid of
    // the old and new machine. Rewind passes transactional=false for its own
    // in-memory frames, avoiding a full duplicate capture during scrubbing.
    std::vector<uint8_t> rollback;
    {
        SnapshotWriter w(rollback);
        captureMachineState(w, cpu, mem, /*includeSlots=*/true);
        if (!w.finish())
            return { false, "cannot create snapshot rollback checkpoint" };
    }

    RestoreResult result = applyMachineState(r, cpu, mem);
    if (result.ok) return result;

    SnapshotReader before(rollback.data(), rollback.size());
    const RestoreResult rolledBack = applyMachineState(before, cpu, mem);
    if (!rolledBack.ok) {
        return { false, result.error + "; rollback failed: " +
                        rolledBack.error };
    }
    return result;
}

}  // namespace pom2

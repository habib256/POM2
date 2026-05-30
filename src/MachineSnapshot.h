// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MachineSnapshot — the single source of truth for "what a POM2 state
// snapshot contains" and how it is applied back.
//
// It serializes / restores exactly the CPU + RAM state that both the
// AI-control `/snapshot/save|load` HTTP handlers and the rewind ring buffer
// (RewindBuffer) need, so the two never drift. The byte layout is the
// SnapshotIO section roster (see SnapshotIO.h):
//
//   CPU  16 bytes  PC A X Y P SP cpuMode + cycle counter
//   MEM  64 KiB    main RAM (restored through Memory::restoreMainRam, ROM
//                  mirror preserved)
//   MEX  v2 blob   aux RAM + Language-Card RAM + RamWorks banks + paging
//                  soft-switches + DisplayState
//
// Disk / slot-card state is deliberately excluded (per CLAUDE.md). The
// functions take a SnapshotWriter/Reader so the caller chooses the backend
// (file for /snapshot, in-memory vector for rewind).

#ifndef POM2_MACHINE_SNAPSHOT_H
#define POM2_MACHINE_SNAPSHOT_H

#include <string>

class M6502;
class Memory;

namespace pom2 {

class SnapshotWriter;
class SnapshotReader;

/// Serialize the full rewindable machine state into `w` (CPU + MEM + MEX).
void captureMachineState(SnapshotWriter& w, M6502& cpu, Memory& mem);

struct RestoreResult {
    bool        ok = true;
    std::string error;   // populated only when ok == false
};

/// Apply the CPU / MEM / MEX sections found in `r` to `cpu` + `mem`.
/// Tolerant by design (mirrors the original AI-control loader):
///   • CPU honoured only at the full 16-byte length — a short, crafted
///     section is skipped (the "round 10 #3" over-read hardening).
///   • MEM honoured only at exactly 0x10000 bytes, restored through
///     Memory::restoreMainRam so the ROM mirror is preserved.
///   • MEX bounded to 16 MiB; an oversized section aborts the restore with
///     ok == false so HTTP callers can surface a 400. RewindBuffer feeds its
///     own data and never hits this.
/// Unknown / wrong-length sections are skipped (forward compatibility).
RestoreResult restoreMachineState(SnapshotReader& r, M6502& cpu, Memory& mem);

}  // namespace pom2

#endif  // POM2_MACHINE_SNAPSHOT_H

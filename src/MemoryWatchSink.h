// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// MemoryWatchSink — where Memory reports a watched access.
//
// An interface rather than a direct call into `pom2::Debugger` for one
// practical reason: `Memory.cpp` is linked into two dozen test binaries and a
// benchmark, and none of them should have to pull the debugger in behind it.
// Memory holds a pointer that is null in every one of those builds, and the
// only implementor is `Debugger` (see Memory.h § Write watchpoints).

#ifndef POM2_MEMORY_WATCH_SINK_H
#define POM2_MEMORY_WATCH_SINK_H

#include <cstdint>

namespace pom2 {

struct MemoryWatchSink {
    virtual ~MemoryWatchSink() = default;
    /// One access to a watched address. `write` is false for reads, which
    /// nothing delivers today — see Debugger::noteAccess for why.
    virtual void noteAccess(uint16_t addr, uint8_t value, bool write) = 0;
};

}  // namespace pom2

#endif  // POM2_MEMORY_WATCH_SINK_H

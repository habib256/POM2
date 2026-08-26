// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Coordinates host-side debug tooling with the emulation state boundary.
// In particular it owns the memory viewer and enforces its two-phase
// read-under-lock / write-after-unlock contract.

#ifndef POM2_DEBUG_COORDINATOR_H
#define POM2_DEBUG_COORDINATOR_H

#include <memory>

class EmulationController;
class MemoryViewer_ImGui;

namespace pom2 {

class DebugCoordinator
{
public:
    explicit DebugCoordinator(EmulationController& controller);
    ~DebugCoordinator();

    DebugCoordinator(const DebugCoordinator&) = delete;
    DebugCoordinator& operator=(const DebugCoordinator&) = delete;

    MemoryViewer_ImGui& memoryViewer() noexcept;
    const MemoryViewer_ImGui& memoryViewer() const noexcept;

    void renderMemoryViewer(bool& open);

private:
    EmulationController& controller_;
    std::unique_ptr<MemoryViewer_ImGui> memoryViewer_;
};

} // namespace pom2

#endif // POM2_DEBUG_COORDINATOR_H

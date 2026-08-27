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

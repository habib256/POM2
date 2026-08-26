// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "DebugCoordinator.h"

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "MemoryViewer_ImGui.h"

#include "imgui.h"

namespace pom2 {

DebugCoordinator::DebugCoordinator(EmulationController& controller)
    : controller_(controller),
      memoryViewer_(std::make_unique<MemoryViewer_ImGui>(&controller.memory()))
{
    // All edits pass through Memory::memWrite under the same state boundary
    // as CPU stores; the viewer itself never receives an unsafe raw writer.
    memoryViewer_->setWriteCallback([this](uint16_t address, uint8_t value) {
        auto state = controller_.lockState();
        state.memory().memWrite(address, value);
    });
}

DebugCoordinator::~DebugCoordinator() = default;

MemoryViewer_ImGui& DebugCoordinator::memoryViewer() noexcept
{
    return *memoryViewer_;
}

const MemoryViewer_ImGui& DebugCoordinator::memoryViewer() const noexcept
{
    return *memoryViewer_;
}

void DebugCoordinator::renderMemoryViewer(bool& open)
{
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Memory viewer", &open)) {
        {
            auto state = controller_.lockState();
            memoryViewer_->setCmosMode(
                state.cpu().getCpuMode() == M6502::CpuMode::CMOS);
            memoryViewer_->render();
        }
        // The write callback re-enters the non-recursive state lock, so the
        // staged edits must be drained only after the read snapshot unlocks.
        memoryViewer_->flushPendingWrites();
    }
    ImGui::End();
}

} // namespace pom2

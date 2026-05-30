// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Rewind_ImGui — see Rewind_ImGui.h.

#include "Rewind_ImGui.h"

#include "EmulationController.h"
#include "RewindBuffer.h"
#include "imgui.h"

#include <cstdint>
#include <mutex>

namespace pom2 {
namespace {
// POM2_CPU_CLOCK_HZ (14.31818 MHz / 14). Only used to turn an emuCycles
// span into a human-readable seconds figure for the timeline labels.
constexpr double kCpuHz = 1022727.0;
}  // namespace

void Rewind_ImGui::beginScrubIfNeeded(EmulationController& ctrl)
{
    if (scrubbing_) return;
    if (ctrl.rewindBeginScrub()) {
        scrubbing_ = true;
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        cursor_ = ctrl.rewind().empty() ? 0 : ctrl.rewind().size() - 1;
    }
}

void Rewind_ImGui::seekTo(EmulationController& ctrl, long index)
{
    if (index < 0) index = 0;
    const size_t got = ctrl.rewindSeek(static_cast<size_t>(index));
    if (got != RewindBuffer::kNoFrame) cursor_ = got;
}

void Rewind_ImGui::holdRewind(EmulationController& ctrl, size_t frames)
{
    beginScrubIfNeeded(ctrl);
    if (scrubbing_) seekTo(ctrl, static_cast<long>(cursor_) - static_cast<long>(frames));
}

void Rewind_ImGui::releaseHold(EmulationController& ctrl)
{
    if (!scrubbing_) return;
    ctrl.rewindEndAndResume(cursor_);
    scrubbing_ = false;
}

Rewind_ImGui::FrameResult Rewind_ImGui::render(const char* title, bool& open,
                                               EmulationController& ctrl,
                                               float /*deltaSeconds*/)
{
    FrameResult res;

    // Read ring stats once, under the lock.
    bool     enabled = false;
    size_t   count = 0, bytes = 0;
    uint64_t oldest = 0, newest = 0, cursorCycle = 0;
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        RewindBuffer& rb = ctrl.rewind();
        enabled = rb.enabled();
        count   = rb.size();
        bytes   = rb.bytes();
        oldest  = rb.oldestCycle();
        newest  = rb.newestCycle();
        if (scrubbing_ && cursor_ < count) cursorCycle = rb.infoAt(cursor_).cycle;
    }
    if (scrubbing_ && count == 0) scrubbing_ = false;   // ring was cleared
    const bool running    = ctrl.getMode() == EmulationController::Mode::Running;
    const bool haveFrames = count > 0;

    ImGui::SetNextWindowSize(ImVec2(540, 132), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin(title, &open)) { ImGui::End(); return res; }

    // ── Record toggle + buffer stats ─────────────────────────────────────
    bool rec = enabled;
    if (ImGui::Checkbox("Record", &rec)) {
        if (rec) {
            ctrl.rewind().setEnabled(true);
            res.statusMessage = "Rewind: recording on";
        } else {
            if (scrubbing_) { ctrl.rewindEndAndResume(cursor_); scrubbing_ = false; }
            ctrl.rewind().setEnabled(false);
            res.statusMessage = "Rewind: recording off";
        }
    }
    ImGui::SameLine();
    const double spanSec = newest >= oldest ? (newest - oldest) / kCpuHz : 0.0;
    ImGui::Text("%zu frames  ·  %.1f s  ·  %.1f MB",
                count, spanSec, static_cast<double>(bytes) / (1024.0 * 1024.0));
    if (!enabled && !haveFrames)
        ImGui::TextDisabled("Enable Record, run for a moment, then scrub the timeline below.");

    // ── Timeline ──────────────────────────────────────────────────────────
    const int maxIdx = haveFrames ? static_cast<int>(count - 1) : 0;
    int sliderVal = scrubbing_ ? static_cast<int>(cursor_) : maxIdx;
    ImGui::BeginDisabled(!haveFrames);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderInt("##timeline", &sliderVal, 0, maxIdx, "")) {
        beginScrubIfNeeded(ctrl);
        seekTo(ctrl, sliderVal);
    }
    ImGui::EndDisabled();

    if (scrubbing_) {
        const double back = newest >= cursorCycle ? (newest - cursorCycle) / kCpuHz : 0.0;
        ImGui::Text("Scrubbing  -%.2f s   (frame %zu / %zu)", back, cursor_ + 1, count);
    } else {
        ImGui::TextDisabled("Live (newest)");
    }

    // ── Transport ─────────────────────────────────────────────────────────
    ImGui::BeginDisabled(!haveFrames);
    if (ImGui::Button("|<"))  { beginScrubIfNeeded(ctrl); seekTo(ctrl, 0); }
    ImGui::SameLine();
    ImGui::Button("<< hold");                          // press-and-hold = live rewind
    if (ImGui::IsItemActive() && haveFrames) {
        beginScrubIfNeeded(ctrl);
        seekTo(ctrl, static_cast<long>(cursor_) - rewindSpeed_);
    }
    ImGui::SameLine();
    if (ImGui::Button("<|"))  { beginScrubIfNeeded(ctrl); seekTo(ctrl, static_cast<long>(cursor_) - 1); }
    ImGui::SameLine();
    if (ImGui::Button("|>"))  { beginScrubIfNeeded(ctrl); seekTo(ctrl, static_cast<long>(cursor_) + 1); }
    ImGui::SameLine();
    if (ImGui::Button("resume here")) {
        if (scrubbing_) { ctrl.rewindEndAndResume(cursor_); scrubbing_ = false; }
        else ctrl.setMode(EmulationController::Mode::Running);
        res.statusMessage = "Rewind: resumed live";
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (running) {
        if (ImGui::Button("Pause")) ctrl.setMode(EmulationController::Mode::Stopped);
    } else {
        if (ImGui::Button("Play ")) {
            if (scrubbing_) { ctrl.rewindEndAndResume(cursor_); scrubbing_ = false; }
            else ctrl.setMode(EmulationController::Mode::Running);
        }
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    const char* speeds[] = { "1x", "2x", "4x" };
    int speedIdx = (rewindSpeed_ >= 4) ? 2 : (rewindSpeed_ >= 2 ? 1 : 0);
    if (ImGui::Combo("##rwspeed", &speedIdx, speeds, 3))
        rewindSpeed_ = (speedIdx == 2) ? 4 : (speedIdx == 1 ? 2 : 1);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hold-rewind speed");

    ImGui::End();
    return res;
}

}  // namespace pom2

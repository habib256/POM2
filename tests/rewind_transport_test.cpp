// Rewind transport test (Phase 3 — controller integration).
//
// Exercises EmulationController's rewind transport against a REAL worker
// thread, pinning the pieces the UI relies on:
//   1. begin-scrub parks the worker (so a UI restore can't be overrun by an
//      in-flight Running frame);
//   2. while parked, no new frames are captured (the timeline is frozen);
//   3. seek restores exact historical state (cycle counter == frame stamp);
//   4. seekToCycle lands on the right frame;
//   5. resume truncates the abandoned future and runs again.
//
// The guest is a 3-byte `JMP $0800` self-loop so the CPU advances
// deterministically forever (no jam, distinct cycle stamp per frame).

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"
#include "RewindBuffer.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <thread>

namespace {

size_t ringSize(EmulationController& ctrl)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    return ctrl.rewind().size();
}

uint64_t frameCycle(EmulationController& ctrl, size_t i)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    return ctrl.rewind().infoAt(i).cycle;
}

uint64_t liveCycle(EmulationController& ctrl)
{
    std::lock_guard<std::mutex> lk(ctrl.stateMutex());
    return ctrl.memory().getCycleCounter();
}

// Poll until the ring holds at least `target` frames, or timeout.
bool waitForFrames(EmulationController& ctrl, size_t target, int timeoutMs)
{
    for (int i = 0; i < timeoutMs; ++i) {
        if (ringSize(ctrl) >= target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

}  // namespace

int main()
{
    EmulationController ctrl;

    // Install a deterministic JMP-self loop at $0800 and point the CPU at it.
    {
        std::lock_guard<std::mutex> lk(ctrl.stateMutex());
        Memory& mem = ctrl.memory();
        mem.memWrite(0x0800, 0x4C);   // JMP $0800
        mem.memWrite(0x0801, 0x00);
        mem.memWrite(0x0802, 0x08);
        ctrl.cpu().setProgramCounter(0x0800);
    }

    ctrl.rewind().setMaxFrames(120);
    ctrl.rewind().setKeyframeInterval(8);   // force a mix of keyframes + deltas
    ctrl.rewind().setEnabled(true);

    ctrl.start();   // spawns the worker, arms Running

    // (1) Frames accrue while running.
    assert(waitForFrames(ctrl, 12, 4000) && "worker never captured frames");

    // (1) begin-scrub parks the worker.
    assert(ctrl.rewindBeginScrub());
    assert(ctrl.rewindIsParked() && "worker did not park after beginScrub");

    // (2) parked → timeline frozen.
    const size_t frozen = ringSize(ctrl);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    assert(ringSize(ctrl) == frozen && "ring grew while parked (capture not gated by run state)");
    assert(frozen >= 12);

    // (3) seek restores exact state: live cycle counter == the frame's stamp.
    for (size_t k : { size_t(0), frozen / 2, frozen - 1 }) {
        const size_t got = ctrl.rewindSeek(k);
        assert(got == k);
        assert(liveCycle(ctrl) == frameCycle(ctrl, k));
    }

    // (4) seekToCycle lands on the newest frame at-or-before the target.
    {
        const size_t mid = frozen / 2;
        const uint64_t target = frameCycle(ctrl, mid) + 1;   // just after frame `mid`
        const size_t got = ctrl.rewindSeekToCycle(target);
        assert(got == mid);
        assert(liveCycle(ctrl) == frameCycle(ctrl, mid));
    }

    // (5) resume from a mid point truncates the future and runs again.
    const size_t resumeAt = frozen / 2;
    const uint64_t resumeCycle = frameCycle(ctrl, resumeAt);
    ctrl.rewindEndAndResume(resumeAt);
    assert(ctrl.getMode() == EmulationController::Mode::Running);
    // The machine continues from the resumed cycle, so it must climb past it.
    bool advanced = false;
    for (int i = 0; i < 2000; ++i) {
        if (liveCycle(ctrl) > resumeCycle) { advanced = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    assert(advanced && "machine did not resume after rewindEndAndResume");
    // And it keeps recording the new timeline.
    const size_t afterResume = ringSize(ctrl);
    assert(waitForFrames(ctrl, afterResume + 3, 2000) && "capture did not resume");

    ctrl.stop();   // dtor also joins, but be explicit
    std::printf("Rewind transport: OK (park + frozen + seek + seekToCycle + resume)\n");
    return 0;
}

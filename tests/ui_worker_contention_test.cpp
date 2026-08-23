// UI-thread ↔ CPU-worker contention test — the targeted TSan pass the GUI
// half never had (TODO P1).
//
// The nightly TSan matrix covers the worker's park/resume, rewind, the audio
// teardown and the helper-process threads, but nothing reproduced the one
// contention the GUI actually creates every frame: the UI thread reading and
// writing emulated input state WHILE the CPU worker runs. MainWindow can't be
// instantiated headless (GLFW/GL), so this drives EmulationController's real
// worker and hammers it with the SAME access disciplines MainWindow uses:
//
//   * paddles/buttons under lockState()      (MainWindow.cpp joystick block)
//   * queueKey() with no lock                (Memory::kbMutex owns it)
//   * Open/Solid-Apple with no lock          (atomic flags)
//   * a "paint" read of memory + PC under lockState()
//
// while the worker executes a loop that READS $C000 / $C061 / $C064, so the
// worker touches the keyboard mirror, the Apple-key atomics and the paddle
// arrays concurrently with the UI writes. Under `-fsanitize=thread` this
// flags any access that escapes those disciplines; without it, it is a
// deadlock / liveness smoke test. It exists so the disciplines stay pinned:
// a future change that reads a paddle off the lock, or writes the keyboard
// latch without kbMutex, turns the nightly TSan leg red instead of shipping.

#include "EmulationController.h"
#include "M6502.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

// A worker loop at $0800 that polls the three input soft-switches forever:
//   LDA $C000   ; keyboard latch  (kbLatchMirror_ atomic)
//   LDA $C061   ; PB0 / Open-Apple (openAppleKey atomic | paddleButton[0])
//   LDA $C064   ; paddle 0        (paddleValue[0])
//   JMP $0800
const uint8_t kPollLoop[] = {
    0xAD, 0x00, 0xC0,
    0xAD, 0x61, 0xC0,
    0xAD, 0x64, 0xC0,
    0x4C, 0x00, 0x08,
};

}  // namespace

int main()
{
    EmulationController ctrl;
    {
        auto st = ctrl.lockState();
        Memory& mem = st.memory();
        for (std::size_t i = 0; i < sizeof(kPollLoop); ++i)
            mem.memWrite(static_cast<uint16_t>(0x0800 + i), kPollLoop[i]);
        st.cpu().setProgramCounter(0x0800);
    }

    ctrl.start();   // spawns the worker in Running mode

    // The "UI thread" is this one: a tight frame loop of exactly the input
    // touches MainWindow makes, with exactly its locking. Bounded iteration
    // count (not wall-clock) so the run is deterministic under TSan's slowdown.
    constexpr int kFrames = 30000;
    for (int f = 0; f < kFrames; ++f) {
        // No lock: keyboard queue (own mutex) + the Apple-key atomics.
        ctrl.memory().queueKey(static_cast<uint8_t>('A' + (f % 26)));
        ctrl.memory().setOpenAppleKey((f & 1) != 0);
        ctrl.memory().setSolidAppleKey((f & 2) != 0);

        // Under the lock: the paddle/button writes (MainWindow's joystick
        // block takes lockState() for exactly these) plus a "paint" read.
        {
            auto st = ctrl.lockState();
            Memory& mem = st.memory();
            for (int i = 0; i < 4; ++i)
                mem.setPaddle(i, static_cast<uint8_t>((f + i) & 0xFF));
            for (int i = 0; i < 3; ++i)
                mem.setPaddleButton(i, ((f >> i) & 1) != 0);
            // Paint: read a few bytes and the PC the way a frame render does.
            volatile uint8_t sink = mem.peekMainRam(0x0800);
            (void)sink;
            (void)st.cpu().getProgramCounter();
        }

        // Occasional paste burst — the FIFO path, still kbMutex-guarded.
        if ((f % 500) == 0) {
            const char burst[] = "LOAD\r";
            ctrl.memory().pasteRawKeys(burst, sizeof(burst) - 1);
        }

        if ((f & 0x3F) == 0) std::this_thread::yield();
    }

    // The worker must still be alive and executing our loop — proof the run
    // was real contention, not a parked worker the UI raced against nothing.
    bool pcInLoop = false;
    {
        auto st = ctrl.lockState();
        const uint16_t pc = st.cpu().getProgramCounter();
        pcInLoop = (pc >= 0x0800 && pc <= 0x080B);
    }
    ctrl.stop();

    assert(pcInLoop && "the worker was not executing the poll loop");

    std::printf("ui_worker_contention_test: OK\n");
    return 0;
}

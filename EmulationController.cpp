// POM2 Apple II Emulator
// Copyright (C) 2026

#include "EmulationController.h"
#include "CpuClock.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>

EmulationController::EmulationController()
    : processor(&mem)
{
    cyclesPerFrame.store(POM2_CPU_CYCLES_PER_FRAME_60HZ);

    // Audio device first — we want its negotiated sample rate before the
    // cassette starts streaming. miniaudio sometimes negotiates 48 kHz on
    // Apple Silicon even when 44.1 kHz is requested; using a stale rate
    // would drift the cassette pulse playback by the rate ratio.
    audioDev = std::make_unique<AudioDevice>();
    tape     = std::make_unique<CassetteDevice>();
    tape->setAudioOutputSampleRate(audioDev->getActualSampleRate());
    tape->setAudioAvailable(audioDev->isAvailable());
    if (audioDev->isAvailable()) audioDev->addSource(tape.get());

    // 1-bit speaker: cycle-driven AudioSource. Sample rate must match
    // the device's negotiated rate or the audio drifts.
    spk = std::make_unique<SpeakerDevice>();
    spk->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(spk.get());

    // Disk II mechanical sounds. Samples are loaded later by MainWindow
    // once the roms/floppy_samples/ directory has been probed; the source
    // is silent until then. Loading at audio-construction time would
    // require EmulationController to know about a roms/ filesystem
    // convention that is otherwise MainWindow's concern.
    floppy = std::make_unique<FloppySoundDevice>();
    floppy->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(floppy.get());

    // //c / //c+ on-board IWM. Memory mirrors $C0E0-$C0EF accesses to
    // this device on iicHasAltBank profiles so its state machine
    // (MAME-faithful, see `IWMDevice.{h,cpp}`) evolves in lock-step
    // with the slot-6 DiskIICard's lightweight shadow. On II/II+/IIe/
    // /16K-//c profiles the pointer is set but never consulted (the
    // iicHasAltBank guard in Memory keeps the mirror off).
    iwmDev = std::make_unique<pom2::IWMDevice>();

    // Wire $C020 / $C060 (cassette) and $C030 (speaker, with sub-
    // instruction timestamping via the CPU back-pointer).
    mem.setCassetteDevice(tape.get());
    mem.setSpeakerDevice(spk.get());
    mem.setCpu(&processor);
    mem.setIWM(iwmDev.get());
}

EmulationController::~EmulationController()
{
    exitRequested.store(true);
    wakeCv.notify_all();
    if (worker.joinable()) worker.join();

    // Tear down audio first so the callback thread is drained before the
    // sources it's pulling from go away.
    if (audioDev && tape)   audioDev->removeSource(tape.get());
    if (audioDev && spk)    audioDev->removeSource(spk.get());
    if (audioDev && floppy) audioDev->removeSource(floppy.get());
    audioDev.reset();
    mem.setCassetteDevice(nullptr);
    mem.setSpeakerDevice(nullptr);
    mem.setCpu(nullptr);
    mem.setIWM(nullptr);
    tape.reset();
    spk.reset();
    floppy.reset();
    iwmDev.reset();
}

// ─── Cassette transport ───────────────────────────────────────────────────

bool EmulationController::loadTape(const std::string& path)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    return tape->loadTape(path);
}
bool EmulationController::saveTape(const std::string& path)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    return tape->saveTape(path);
}
void EmulationController::playTape()         { std::lock_guard<std::mutex> lk(stateMtx); tape->playTape(); }
void EmulationController::stopTape()         { std::lock_guard<std::mutex> lk(stateMtx); tape->stopTape(); }
void EmulationController::pauseTape(bool p)  { std::lock_guard<std::mutex> lk(stateMtx); tape->setPlaybackPaused(p); }
void EmulationController::rewindTape()       { std::lock_guard<std::mutex> lk(stateMtx); tape->rewindTape(); }
void EmulationController::ejectTape()        { std::lock_guard<std::mutex> lk(stateMtx); tape->ejectTape(); }
void EmulationController::clearTapeCapture() { std::lock_guard<std::mutex> lk(stateMtx); tape->clearRecordedTape(); }
void EmulationController::seekTapeRelative(double dt)
{
    std::lock_guard<std::mutex> lk(stateMtx);
    tape->seekRelativeSeconds(dt);
}
void EmulationController::setCassetteVolume(float v) { tape->setVolume(v); }

void EmulationController::start()
{
    if (worker.joinable()) return;
    worker = std::thread([this] { workerLoop(); });
}

void EmulationController::stop()
{
    setMode(Mode::Stopped);
}

void EmulationController::hardReset()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (spk)    spk->reset();
    if (iwmDev) iwmDev->reset();
    processor.hardReset();
    pom2::log().info("Emul", "Hard reset");
}

void EmulationController::softReset()
{
    // Real Apple II Ctrl-Reset: the reset line is asserted briefly. The
    // 6502 latches PC from $FFFC, sets the I flag, and rewinds SP to $FF.
    // RAM, A/X/Y, and the zero page survive. Slot cards see their reset
    // line too — Disk II spins down, Le Chat Mauve FIFO returns to its
    // power-on default, etc. Soft-switch state is reset by the Apple II
    // ROM's autostart code (SETVID/SETKBD called from the reset handler),
    // which we mirror here so behaviour matches with or without ROM loaded.
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    processor.softReset();
    pom2::log().info("Emul", "Soft reset (Ctrl-Reset)");
}

void EmulationController::coldBoot()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (iwmDev) iwmDev->reset();
    processor.hardReset();
    pom2::log().info("Emul", "Cold boot (RAM wiped)");
}

void EmulationController::bootFromSlot(int slot)
{
    if (slot < 1 || slot > 7) return;
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (spk)    spk->reset();
    if (iwmDev) iwmDev->reset();
    // Prime text page 1 with $A0 (space + high bit set) — what the Monitor
    // ROM's HOME routine would write. We force PC into the slot ROM here
    // instead of going through the Monitor cold-boot (which would scan
    // slots and pick slot 6 if a floppy is mounted there), so the user
    // would otherwise see freshly-zeroed RAM render as a screen full of
    // `@`-tile garbage for several seconds while the booting card loads.
    for (uint16_t a = 0x0400; a <= 0x07FF; ++a) {
        mem.memWrite(a, 0xA0);
    }
    processor.hardReset();
    processor.setProgramCounter(static_cast<uint16_t>(0xC000 + slot * 0x100));
    mode.store(Mode::Running);
    wakeCv.notify_all();
    pom2::log().info("Emul",
        "Boot via slot " + std::to_string(slot) + " ROM ($C" +
        std::to_string(slot) + "00)");
}

void EmulationController::requestStep()
{
    stepRequested.store(true);
    setMode(Mode::Step);
    wakeCv.notify_all();
}

void EmulationController::setMode(Mode m)
{
    mode.store(m);
    wakeCv.notify_all();
}

void EmulationController::workerLoop()
{
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now();

    while (!exitRequested.load()) {
        const Mode m = mode.load();

        if (m == Mode::Stopped) {
            // Idle wait. Wake on any state change so toggling Run is snappy.
            std::unique_lock<std::mutex> lk(stateMtx);
            wakeCv.wait_for(lk, std::chrono::milliseconds(50),
                [this]{ return exitRequested.load() ||
                               mode.load() != Mode::Stopped; });
            nextTick = clock::now();
            continue;
        }

        if (m == Mode::Step) {
            if (stepRequested.exchange(false)) {
                std::lock_guard<std::mutex> lk(stateMtx);
                processor.step();
                // M6502::step() already calls memory->advanceCycles(cycles)
                // with the *current instruction's* cycle count — per-step
                // accounting is canonical so cassette + speaker + slot
                // peripherals stay cycle-aligned. Calling it again here
                // would double-count (which is exactly the bug that made
                // the speaker tone freeze: cycleCounter drifted ahead of
                // wallclock, the audio cursor lagged 200 ms+, the catch-up
                // logic dropped events in a loop, and the synth got stuck
                // on whatever level survived the drop).
            }
            mode.store(Mode::Stopped);
            continue;
        }

        // Running: execute one frame's worth of cycles, then sleep until
        // the next 60 Hz boundary. Using steady_clock keeps wallclock pace
        // without drifting on busy machines.
        //
        // We chunk the budget into small pieces and release `stateMtx`
        // between each — the UI thread takes that mutex many times per
        // render pass (display snapshot, every panel's state read,
        // every menu click handler), and even one ~25 ms hold under
        // turbo (cyclesPerFrame = 1M with the heavier IIe firmware
        // dispatch) was enough to drop the GUI to a few fps. A 4096-
        // cycle chunk is < 0.1 ms wall on Apple Silicon, so the UI
        // gets the lock between chunks within a frame's UI budget —
        // ~250 chunks per emulated turbo frame, ~50 µs of pure
        // lock/unlock overhead per frame, which is invisible.
        //
        // Smaller chunks would mostly amortise into wasted contention;
        // larger chunks (we tried 16K first) leave the UI noticeably
        // laggy during long disk reads.
        constexpr int kLockChunkCycles = 4096;
        const int budget = cyclesPerFrame.load();
        for (int done = 0; done < budget; ) {
            const int chunk = std::min(kLockChunkCycles, budget - done);
            std::lock_guard<std::mutex> lk(stateMtx);
            const int actually = processor.run(chunk);
            done += (actually > 0 ? actually : chunk);
            // No mem.advanceCycles here — see Step branch above.
        }
        nextTick += std::chrono::microseconds(1'000'000 / 60);
        const auto now = clock::now();
        if (now < nextTick) {
            std::this_thread::sleep_for(nextTick - now);
        } else if (now - nextTick > std::chrono::milliseconds(100)) {
            // We fell behind — resync rather than try to catch up forever.
            nextTick = now;
        }
    }
}

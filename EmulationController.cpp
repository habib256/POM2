// POM2 Apple II Emulator
// Copyright (C) 2026

#include "EmulationController.h"
#include "CpuClock.h"
#include "Logger.h"

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

    // Wire $C020 / $C060 (cassette) and $C030 (speaker, with sub-
    // instruction timestamping via the CPU back-pointer).
    mem.setCassetteDevice(tape.get());
    mem.setSpeakerDevice(spk.get());
    mem.setCpu(&processor);
}

EmulationController::~EmulationController()
{
    exitRequested.store(true);
    wakeCv.notify_all();
    if (worker.joinable()) worker.join();

    // Tear down audio first so the callback thread is drained before the
    // sources it's pulling from go away.
    if (audioDev && tape) audioDev->removeSource(tape.get());
    if (audioDev && spk)  audioDev->removeSource(spk.get());
    audioDev.reset();
    mem.setCassetteDevice(nullptr);
    mem.setSpeakerDevice(nullptr);
    mem.setCpu(nullptr);
    tape.reset();
    spk.reset();
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
    if (spk) spk->reset();
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
    if (spk) spk->reset();
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
        const int budget = cyclesPerFrame.load();
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            (void)processor.run(budget);
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

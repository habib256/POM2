// POM2 Apple II Emulator
// Copyright (C) 2026

#include "EmulationController.h"
#include "CpuClock.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>

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

    // Floppy mechanical sounds — two independent instances so the 5.25"
    // and 3.5" sample sets coexist (FloppySoundDevice stores a single
    // sample bank per instance). Samples are loaded later by MainWindow
    // once the roms/floppy_samples/ directory has been probed; both
    // sources are silent until then. Loading at audio-construction time
    // would require EmulationController to know about a roms/ filesystem
    // convention that is otherwise MainWindow's concern.
    floppy525 = std::make_unique<FloppySoundDevice>();
    floppy525->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(floppy525.get());

    floppy35 = std::make_unique<FloppySoundDevice>();
    floppy35->setSampleRate(audioDev->getActualSampleRate());
    if (audioDev->isAvailable()) audioDev->addSource(floppy35.get());

    // //c / //c+ on-board IWM. Memory mirrors $C0E0-$C0EF accesses to
    // this device on iicHasAltBank profiles so its state machine
    // (MAME-faithful, see `IWMDevice.{h,cpp}`) evolves in lock-step
    // with the slot-6 DiskIICard's lightweight shadow. On II/II+/IIe/
    // /16K-//c profiles the pointer is set but never consulted (the
    // iicHasAltBank guard in Memory keeps the mirror off).
    iwmDev = std::make_unique<pom2::IWMDevice>();
    // Optional opt-in: route $C0EC/ED/EE/EF *reads* through the IWM
    // on iicHasAltBank profiles. Off by default (shadow mode) so the
    // //c+ boot path keeps working via DiskIICard's LSS reads while
    // the IWM bit-cell window walker is still being tuned against
    // POM2's flux-cell timing. Set `POM2_IWM_AUTHORITATIVE=1` to
    // exercise the MAME-faithful IWM data path end-to-end.
    if (const char* env = std::getenv("POM2_IWM_AUTHORITATIVE")) {
        mem.setIWMAuthoritative(env[0] != '0');
    }

    // //c+ SmartPort 3.5" hub. Holds the two Sony 3.5" drive objects
    // plus the drive-selection state machine. MIG state changes
    // (Memory $C0CC / $C0CE windows on bank-1 alt firmware) route
    // through it; the IWM's phases/devsel callbacks are wired here so
    // 3.5" drives receive command strobes. Off-path on II/II+/IIe/
    // 16K-//c profiles — the hub is constructed but Memory never
    // routes traffic into it unless `iicHasAltBank` is set.
    image35Int = std::make_unique<pom2::Disk35Image>();
    image35Ext = std::make_unique<pom2::Disk35Image>();
    drive35Int = std::make_unique<pom2::Sony35Drive>();
    drive35Ext = std::make_unique<pom2::Sony35Drive>();
    hub        = std::make_unique<pom2::SmartPortHub>();
    drive35Int->setImage(image35Int.get());
    drive35Ext->setImage(image35Ext.get());
    // Mechanical-sound source: dedicated 3.5" `FloppySoundDevice`
    // instance, loaded with the `35_*.wav` Sony sample set. Step cadence
    // + motor on/off are driven from `Sony35Drive::strobeWriteRegister`
    // cases 0x1 / 0x2 / 0x3, stamped with the IWM's last-tick CPU cycle
    // so the audio thread can measure cadence in emulated time (matches
    // the comment block on FloppySoundSink::step). Samples are loaded
    // later by MainWindow from roms/floppy_samples/ — until then the
    // 3.5" channel stays silent.
    drive35Int->setFloppySound(floppy35.get());
    drive35Ext->setFloppySound(floppy35.get());
    hub->attach(iwmDev.get());
    hub->setSony35(drive35Int.get(), drive35Ext.get());

    // Wire $C020 / $C060 (cassette) and $C030 (speaker, with sub-
    // instruction timestamping via the CPU back-pointer).
    mem.setCassetteDevice(tape.get());
    mem.setSpeakerDevice(spk.get());
    mem.setCpu(&processor);
    mem.setIWM(iwmDev.get());
    mem.setSmartPortHub(hub.get());
}

EmulationController::~EmulationController()
{
    exitRequested.store(true);
    wakeCv.notify_all();
    if (worker.joinable()) worker.join();

    // Tear down audio first so the callback thread is drained before the
    // sources it's pulling from go away.
    if (audioDev && tape)      audioDev->removeSource(tape.get());
    if (audioDev && spk)       audioDev->removeSource(spk.get());
    if (audioDev && floppy525) audioDev->removeSource(floppy525.get());
    if (audioDev && floppy35)  audioDev->removeSource(floppy35.get());
    audioDev.reset();
    mem.setCassetteDevice(nullptr);
    mem.setSpeakerDevice(nullptr);
    mem.setCpu(nullptr);
    mem.setIWM(nullptr);
    mem.setSmartPortHub(nullptr);
    tape.reset();
    spk.reset();
    floppy525.reset();
    floppy35.reset();
    iwmDev.reset();
    // Order matters: hub holds raw pointers to the drives, drives hold
    // raw pointers to the images. Tear down in reverse-attach order so
    // no dangling pointers escape into the audio/UI thread.
    hub.reset();
    drive35Int.reset();
    drive35Ext.reset();
    image35Int.reset();
    image35Ext.reset();
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

// ─── 3.5" Sony SmartPort ──────────────────────────────────────────────────
// MAME `apple2e.cpp:4521-4524` instantiates `m_floppy[2..3]` as `add_35()`
// drives. POM2 collapses the "drive" + "image" into a single mount call
// per slot index.

bool EmulationController::mount35(int idx, const std::string& path)
{
    if (idx < 0 || idx > 1) return false;
    std::lock_guard<std::mutex> lk(stateMtx);
    pom2::Disk35Image*  image = idx == 0 ? image35Int.get() : image35Ext.get();
    pom2::Sony35Drive*  drive = idx == 0 ? drive35Int.get() : drive35Ext.get();
    if (!image || !drive) return false;
    image->eject();
    if (!image->loadFile(path)) {
        drive->notifyMediaChange();
        return false;
    }
    drive->notifyMediaChange();
    // User-initiated mount → one-shot insert click. Same pattern as
    // `DiskIICard::insertDisk` (5.25"). Silent when no FloppySoundDevice
    // sink is wired (headless / tests).
    drive->emitInsertClick();
    return true;
}

void EmulationController::eject35(int idx)
{
    if (idx < 0 || idx > 1) return;
    std::lock_guard<std::mutex> lk(stateMtx);
    pom2::Disk35Image* image = idx == 0 ? image35Int.get() : image35Ext.get();
    pom2::Sony35Drive* drive = idx == 0 ? drive35Int.get() : drive35Ext.get();
    if (image && image->isLoaded()) {
        // Persist firmware-driven write-backs before the slot drops the
        // payload. Mirrors `DiskIICard::ejectDisk` for 5.25". Silent
        // on `saveDirty` failure — the panel surfaces the error on the
        // next mount attempt via `Disk35Image::lastError`.
        if (image->hasUnsavedChanges() && !image->isWriteProtected()) {
            image->saveDirty();
        }
        image->eject();
        if (drive) {
            drive->notifyMediaChange();
            // Mechanical click on user-initiated eject — pairs with
            // the click emitted by `mount35` above.
            drive->emitInsertClick();
        }
    }
}

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
    if (hub)    hub->reset();
    processor.hardReset();
    pom2::log().info("Emul", "Hard reset");
}

void EmulationController::softReset()
{
    // Real Apple II Ctrl-Reset: the reset line is asserted briefly. The
    // 6502 latches PC from $FFFC, sets the I flag, and decrements SP by 3
    // (the reset sequence simulates a BRK push of PC + P without storing).
    // RAM, A/X/Y, and the zero page survive. Slot cards see their reset
    // line too — Disk II spins down, Le Chat Mauve FIFO returns to its
    // power-on default, etc. Soft-switch policy depends on the profile:
    // II/II+ machine_reset preserves LC + display (MAME `apple2.cpp:325-
    // 331`); IIe/IIc/IIc+ reset_w wipes the MMU/IOU/LC list (MAME
    // `apple2e.cpp:1453-1508`). resetSoftSwitchesWarm() applies the
    // right one based on iieMode.
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.resetSoftSwitchesWarm();
    mem.slotBus().reset();
    // SmartPort hub state — MAME re-asserts m_35sel=false and
    // m_intdrive=false on every reset (`apple2e.cpp:1266`). Without
    // this (E-3-1), Ctrl-Reset would keep the //c+ alt firmware's
    // last drive selection across the warm-reset boundary.
    if (hub) hub->reset();
    processor.softReset();
    pom2::log().info("Emul", "Soft reset (Ctrl-Reset)");
}

void EmulationController::coldBoot()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (spk)    spk->reset();   // F-1-3: parity with hardReset
    if (iwmDev) iwmDev->reset();
    if (hub)    hub->reset();
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
    if (hub)    hub->reset();
    // Apple II autostart signature (Apple II Ref Manual Appx C): a
    // bootable card's $Cn00 PROM begins with `$20 ?? $00 $03 $3C` at
    // offsets 1/3/5/7. The Autostart F8 firmware refuses to JSR a
    // slot that doesn't carry this signature. We replicate the check
    // here so bootFromSlot doesn't silently land in garbage when the
    // user clicks "Boot" on a slot whose card has no boot PROM
    // (B-2-2). MAME doesn't model the scan in C++ but the firmware
    // does the same validation natively.
    const uint16_t cnxx = static_cast<uint16_t>(0xC000 + slot * 0x100);
    const uint8_t b1 = mem.memRead(static_cast<uint16_t>(cnxx + 1));
    const uint8_t b3 = mem.memRead(static_cast<uint16_t>(cnxx + 3));
    const uint8_t b5 = mem.memRead(static_cast<uint16_t>(cnxx + 5));
    const uint8_t b7 = mem.memRead(static_cast<uint16_t>(cnxx + 7));
    const bool hasBootSignature =
        (b1 == 0x20) && (b3 == 0x00) && (b5 == 0x03) && (b7 == 0x3C);
    if (!hasBootSignature) {
        pom2::log().warn("Emul",
            "Slot " + std::to_string(slot) +
            " has no Apple-II boot signature at $Cn00 — autostart firmware "
            "would skip it. Falling back to cold boot so the F8 ROM can "
            "scan for a bootable slot.");
        // Re-do RAM wipe via coldBoot's path (we already wiped above, so
        // just trigger the reset + autostart path).
        processor.hardReset();
        mode.store(Mode::Running);
        wakeCv.notify_all();
        return;
    }
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
    processor.setProgramCounter(cnxx);
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
        // Pulse the IWM once per frame so its 1-emulated-second
        // drive-disable timer (MAME `iwm.cpp:70-84 update_timer_tick`)
        // still drains when the //c+ alt firmware stops poking
        // $C0Ex between disk operations. Cheap when idle — the
        // `!active_` early-out in `sync` short-circuits.
        if (iwmDev) {
            std::lock_guard<std::mutex> lk(stateMtx);
            iwmDev->tick(mem.getCycleCounter());
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

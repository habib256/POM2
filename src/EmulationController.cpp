// POM2 Apple II Emulator
// Copyright (C) 2026

#include "EmulationController.h"
#include "CpuClock.h"
#include "Logger.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
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

    // Dallas DS1216E "No-Slot Clock". Battery-backed on real hardware,
    // so we hold it at controller scope; survives profile switches and
    // CPU resets. Memory hooks the $F800-$FFFF intercept through this
    // pointer; nullptr disables (MainWindow toggles via setEnabled()).
    noSlotClock_ = std::make_unique<pom2::NoSlotClock>();

    // Wire $C020 / $C060 (cassette) and $C030 (speaker, with sub-
    // instruction timestamping via the CPU back-pointer).
    mem.setCassetteDevice(tape.get());
    mem.setSpeakerDevice(spk.get());
    mem.setCpu(&processor);
    mem.setIWM(iwmDev.get());
    mem.setSmartPortHub(hub.get());
    mem.setNoSlotClock(noSlotClock_.get());
}

EmulationController::~EmulationController()
{
    exitRequested.store(true);
    wakeCv.notify_all();
#ifndef __EMSCRIPTEN__
    if (worker.joinable()) worker.join();
#endif

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
    mem.setNoSlotClock(nullptr);
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
void EmulationController::armRecording()     { std::lock_guard<std::mutex> lk(stateMtx); tape->armRecording(); }
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
    // "Start" means "make the machine run": resume execution AND, on the
    // first call, spawn the worker thread. Re-arming the mode here is the
    // load-bearing part — applyProfile() and restartEmulationFromSettings()
    // both do stop()…rebuild cards…start() while the worker is ALREADY
    // live, so start() can't re-spawn the thread (worker.joinable() short-
    // circuits below). Without setting Running here the machine would stay
    // Stopped after a profile/slot switch and sit frozen on a garbage
    // (HOME-never-ran) text page — the "doesn't boot on launch" bug, since
    // a saved non-default profile auto-applies on startup. Keeps stop()/
    // start() symmetric: stop() parks the mode, start() un-parks it.
    mode.store(Mode::Running);
    wakeCv.notify_all();
#ifndef __EMSCRIPTEN__
    if (worker.joinable()) return;
    worker = std::thread([this] { workerLoop(); });
#endif
    // Under Emscripten the browser owns the frame schedule — the host
    // calls tickFrame() once per RAF. No worker thread is spawned.
}

void EmulationController::tickFrame()
{
    const Mode m = mode.load();
    if (m == Mode::Stopped) {
        return;
    }
    if (m == Mode::Step) {
        const int n = stepsPending.exchange(0);
        if (n > 0) {
            std::lock_guard<std::mutex> lk(stateMtx);
            for (int i = 0; i < n; ++i) {
                processor.step();   // step() runs mem.advanceCycles internally
            }
        }
        mode.store(Mode::Stopped);
        return;
    }
    // Mode::Running — same chunking as workerLoop (4 KiB cycles per
    // lock-hold) so any host code that grabs stateMtx between frames
    // still gets fair access mid-budget. On WASM there's only one
    // thread, but the lock is cheap and keeps the lock-discipline
    // contract identical to the threaded path.
    constexpr int kLockChunkCycles = 4096;
    const int64_t budget = cyclesPerFrame.load();  // int64: see workerLoop note
    for (int64_t done = 0; done < budget; ) {
        const int chunk = static_cast<int>(std::min<int64_t>(kLockChunkCycles, budget - done));
        std::lock_guard<std::mutex> lk(stateMtx);
        const int actually = processor.run(chunk);
        done += (actually > 0 ? actually : chunk);
    }
    if (iwmDev) {
        std::lock_guard<std::mutex> lk(stateMtx);
        iwmDev->tick(mem.getCycleCounter());
    }
}

void EmulationController::stop()
{
    setMode(Mode::Stopped);
}

void EmulationController::hardReset()
{
    std::lock_guard<std::mutex> lk(stateMtx);
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
    // Same soft-switch policy as softReset (MAME reset_w / machine_reset):
    // II/II+ preserve LC + display switches; IIe-class wipes MMU/IOU/LC.
    // Pre-fix hardReset called resetSoftSwitches() unconditionally, which
    // forced TEXT+no NTSC on every F12 even on II/II+.
    mem.resetSoftSwitchesWarm();
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
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
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
    mem.setIicSmartPortArmed(false);   // reboot → //c sees its real $C500 firmware
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
    // Explicit GUI/CLI boot — arm the //c-class on-board SmartPort so its
    // $C500 firmware stub becomes visible for the signature check + boot
    // below (every reset/cold-boot disarms it again). See Memory::
    // setIicSmartPortArmed + project_iic_smartport_boot. No-op off //c-class.
    mem.setIicSmartPortArmed(true);
    mem.clearRam();
    mem.resetSoftSwitches();
    mem.slotBus().reset();
    if (spk)    spk->reset();
    if (iwmDev) iwmDev->reset();
    if (hub)    hub->reset();
    // Card-has-boot-entry sanity check. Apple II Ref Manual Appx C
    // describes 4 signature bytes ($Cn01=$20, $Cn03=$00, $Cn05=$03,
    // $Cn07=$3C); the F8 Autostart Monitor (Apple part 341-0020-00)
    // checks ALL four and only auto-scans Disk II / SmartPort
    // ($Cn07=$3C). HDV uses $Cn07=$01 (ProDOS non-removable) and
    // SmartPort uses $Cn07=$3C; both carry the JSR-dispatch trio at
    // $Cn01/03/05. The user-initiated "Boot" GUI button bypasses the
    // F8 scan deliberately — it's the WHOLE POINT of bootFromSlot
    // (the F8 firmware refuses to scan HDV cards, so the user clicking
    // "Boot HDV" needs this shortcut). We therefore validate only the
    // JSR trio, NOT the $Cn07=$3C marker. Theme 8 audit (gap B-2-2)
    // originally required $Cn07=$3C too, but that broke HDV boot —
    // see DEV.md § Storage § DiskIICard for the bootFromSlot rationale.
    const uint16_t cnxx = static_cast<uint16_t>(0xC000 + slot * 0x100);
    const uint8_t b1 = mem.memRead(static_cast<uint16_t>(cnxx + 1));
    const uint8_t b3 = mem.memRead(static_cast<uint16_t>(cnxx + 3));
    const uint8_t b5 = mem.memRead(static_cast<uint16_t>(cnxx + 5));
    const bool hasBootDispatch =
        (b1 == 0x20) && (b3 == 0x00) && (b5 == 0x03);
    if (!hasBootDispatch) {
        pom2::log().warn("Emul",
            "Slot " + std::to_string(slot) +
            " has no Apple-II JSR-dispatch signature at $Cn01/03/05 — "
            "the card isn't bootable. Falling back to cold boot so the "
            "F8 ROM can scan for a different bootable slot.");
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
    // Replicate the F8 Autostart Monitor's text + I/O zero-page setup. The
    // normal boot reaches the slot ROM via the Monitor cold-start (SETNORM/
    // INIT/SETTXT/VTAB), which initialises the text-window and I/O-hook
    // zero page; bootFromSlot jumps straight to $Cn00 and skips it. Without
    // this, a boot's RWTS that calls Monitor screen routines hits garbage:
    // e.g. CLREOP ($FCA0) loops on `CPY WNDWDTH($21)` — with $21 left at 0
    // it overruns the line and clobbers the boot loader in $0800, hanging
    // the Mr. Robot 4am crack (and any timing/screen-sensitive loader).
    // Values verified against a clean autostart-to-BASIC dump.
    mem.memWrite(0x0020, 0x00);   // WNDLFT  = 0
    mem.memWrite(0x0021, 0x28);   // WNDWDTH = 40
    mem.memWrite(0x0022, 0x00);   // WNDTOP  = 0
    mem.memWrite(0x0023, 0x18);   // WNDBTM  = 24
    mem.memWrite(0x0024, 0x00);   // CH (cursor column)
    mem.memWrite(0x0025, 0x00);   // CV (cursor row)
    mem.memWrite(0x0028, 0x00);   // BASL ┐ text base = $0400 (page 1, row 0)
    mem.memWrite(0x0029, 0x04);   // BASH ┘
    mem.memWrite(0x0032, 0xFF);   // INVFLG = normal video
    mem.memWrite(0x0036, 0xF0);   // CSWL ┐ output hook → COUT1 ($FDF0)
    mem.memWrite(0x0037, 0xFD);   // CSWH ┘
    mem.memWrite(0x0038, 0x1B);   // KSWL ┐ input hook  → KEYIN ($FD1B)
    mem.memWrite(0x0039, 0xFD);   // KSWH ┘
    processor.hardReset();
    processor.setProgramCounter(cnxx);
    mode.store(Mode::Running);
    wakeCv.notify_all();
    pom2::log().info("Emul",
        "Boot via slot " + std::to_string(slot) + " ROM ($C" +
        std::to_string(slot) + "00)");
}

void EmulationController::requestStep(int n)
{
    if (n <= 0) return;
    // Counter, not a boolean: callers (e.g. CLI `--step N`) may queue many
    // steps in a burst; a boolean would coalesce them into a single step.
    stepsPending.fetch_add(n);
    setMode(Mode::Step);
    wakeCv.notify_all();
}

void EmulationController::setMode(Mode m)
{
    mode.store(m);
    wakeCv.notify_all();
}

#ifndef __EMSCRIPTEN__
void EmulationController::workerLoop()
{
    using clock = std::chrono::steady_clock;
    auto nextTick = clock::now();

    // ── Hang detector (POM2_TRACE_HANG=1) ──────────────────────────────────
    // Diagnostic for deterministic freezes (e.g. Nox Archaist hanging when
    // entering a city): if the CPU stays confined to a small PC window for a
    // few seconds it is spinning in a wait loop. We disassemble that loop so
    // the operand reveals exactly which hardware register the game is polling
    // forever ($C019 VBL / $C0EC disk data / $C0D3 HDV status / $C000 kbd …),
    // which pinpoints the missing/incorrect emulation. Near-zero cost when off.
    const bool hangTrace = std::getenv("POM2_TRACE_HANG") != nullptr;
    if (hangTrace) mem.setIoReadTrace(true);
    constexpr int kHangSamples = 180;        // ~3 s at 60 Hz
    uint16_t pcRing[kHangSamples] = {0};
    int  pcRingCount = 0;
    int  pcRingHead  = 0;
    bool hangDumped     = false;
    int  framesConfined = 0;
    auto dumpHang = [&](uint16_t lo, uint16_t hi, bool repeat) {
        std::lock_guard<std::mutex> lk(stateMtx);
        std::fprintf(stderr,
            "\n*** POM2 %s — CPU confined to $%04X..$%04X ***\n",
            repeat ? "STILL FROZEN (permanent loop — this is the real freeze)"
                   : "HANG DETECTED (could be a slow chunk; watch for STILL FROZEN)",
            lo, hi);
        std::fprintf(stderr,
            "  A=%02X X=%02X Y=%02X SP=%02X P=%02X PC=%04X  cycles=%llu\n",
            processor.getAccumulator(), processor.getXRegister(),
            processor.getYRegister(), processor.getStackPointer(),
            processor.getStatusRegister(), processor.getProgramCounter(),
            static_cast<unsigned long long>(mem.getCycleCounter()));
        const uint16_t mm = mem.iieModeFlags();
        std::fprintf(stderr,
            "  //e paging: 80STORE=%d RAMRD=%d RAMWRT=%d INTCXROM=%d ALTZP=%d "
            "80COL=%d (flags=$%04X)\n",
            (mm & Memory::MF_80STORE) ? 1 : 0, (mm & Memory::MF_RAMRD) ? 1 : 0,
            (mm & Memory::MF_RAMWRT) ? 1 : 0, (mm & Memory::MF_INTCXROM) ? 1 : 0,
            (mm & Memory::MF_ALTZP) ? 1 : 0, (mm & Memory::MF_80COL) ? 1 : 0,
            static_cast<unsigned>(mm));
        const uint16_t start = (lo >= 3) ? static_cast<uint16_t>(lo - 3) : 0;
        const uint16_t end   = static_cast<uint16_t>(hi + 8);
        std::fprintf(stderr, "  loop bytes (as currently paged; may be aux RAM):\n");
        for (uint32_t a = start; a <= end; a += 16) {
            std::fprintf(stderr, "    $%04X:", static_cast<unsigned>(a));
            for (uint32_t k = 0; k < 16 && (a + k) <= end; ++k)
                std::fprintf(stderr, " %02X",
                             mem.memRead(static_cast<uint16_t>(a + k)));
            std::fprintf(stderr, "\n");
        }
        std::fprintf(stderr,
            "  recent $C0xx reads (addr(label)xcount, most-polled first):\n    %s\n",
            mem.recentIoReadSummary().c_str());
        // Zero-page pointers used by the decompressor + the buffer it reads,
        // so we can compare the in-RAM compressed data against the .hdv to
        // tell data-corruption (disk/paging bug) from a pure logic/CPU bug.
        const uint8_t z04 = mem.memRead(0x04), z05 = mem.memRead(0x05);
        const uint8_t z06 = mem.memRead(0x06), z07 = mem.memRead(0x07);
        const uint8_t zEA = mem.memRead(0xEA), zEB = mem.memRead(0xEB);
        const uint8_t zEC = mem.memRead(0xEC), zED = mem.memRead(0xED);
        std::fprintf(stderr,
            "  ZP: $04=%02X $05=%02X $06=%02X $07=%02X  out($EA/EB)=%02X%02X"
            "  in($EC/ED)=%02X%02X\n",
            z04, z05, z06, z07, zEB, zEA, zED, zEC);
        const uint16_t inPtr = static_cast<uint16_t>(zEC | (zED << 8));
        std::fprintf(stderr, "  bytes around input ptr $%04X (current paging):\n",
                     inPtr);
        for (int row = -16; row < 32; row += 16) {
            const uint16_t base = static_cast<uint16_t>(inPtr + row);
            std::fprintf(stderr, "    $%04X:", base);
            for (int k = 0; k < 16; ++k)
                std::fprintf(stderr, " %02X",
                             mem.memRead(static_cast<uint16_t>(base + k)));
            std::fprintf(stderr, "\n");
        }
        // Decisive: the SAME input addresses in BOTH physical banks. Tells us
        // whether the real (compressed, non-$00FF) data is in main or aux —
        // i.e. whether the decompressor is reading the WRONG bank, or the data
        // was never written to the bank it expects.
        const uint8_t* mn = mem.data();
        const uint8_t* ax = mem.auxData();
        std::fprintf(stderr, "  input region MAIN vs AUX (which bank holds real data?):\n");
        for (int row = -16; row < 32; row += 16) {
            const uint16_t base = static_cast<uint16_t>(inPtr + row);
            std::fprintf(stderr, "    $%04X main:", base);
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %02X", mn[(base + k) & 0xFFFF]);
            std::fprintf(stderr, "\n            aux :");
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %02X", ax[(base + k) & 0xFFFF]);
            std::fprintf(stderr, "\n");
        }
        std::fflush(stderr);
    };

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
            // Drain ONE queued step per worker iteration, releasing stateMtx
            // between steps (so the UI thread isn't starved during a long
            // `--step N`), and stay in Step mode until the queue empties.
            if (stepsPending.load() > 0) {
                {
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
                stepsPending.fetch_sub(1);
            }
            if (stepsPending.load() <= 0) {
                mode.store(Mode::Stopped);
                // A requestStep() on the UI/CLI thread can race the store
                // above: it may have re-armed Mode::Step and queued a step
                // between our load and this store, which we'd then clobber to
                // Stopped — permanently losing that step. Recover by checking
                // the queue once more and re-arming Step if work appeared.
                if (stepsPending.load() > 0) mode.store(Mode::Step);
            }
            continue;
        }

        // Running: execute one frame's worth of cycles, then sleep until
        // the next 60 Hz boundary. Using steady_clock keeps wallclock pace
        // without drifting on busy machines.
        //
        // Snapshot display state + clear the video-event log before the
        // CPU budget runs so Apple2Display can beam-race mid-frame soft
        // switches when events are present.
        {
            std::lock_guard<std::mutex> lk(stateMtx);
            mem.beginVideoEventFrame();
        }
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
        // int64 accumulator: cyclesPerFrame is capped only at INT_MAX by the
        // CLI, and processor.run() may overshoot `chunk`, so an `int done`
        // could overflow (signed UB) near the ceiling. int64 makes the loop
        // bound safe without restricting the accepted --speed range.
        const int64_t budget = cyclesPerFrame.load();
        for (int64_t done = 0; done < budget; ) {
            const int chunk = static_cast<int>(std::min<int64_t>(kLockChunkCycles, budget - done));
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

        // Hang detector: sample end-of-frame PC; if every sample over the
        // last ~3 s sits inside a small window, the CPU is stuck in a wait
        // loop — dump it once.
        if (hangTrace) {
            const uint16_t pc = processor.getProgramCounter();
            pcRing[pcRingHead] = pc;
            pcRingHead = (pcRingHead + 1) % kHangSamples;
            if (pcRingCount < kHangSamples) ++pcRingCount;
            if (pcRingCount == kHangSamples) {
                uint16_t lo = 0xFFFF, hi = 0;
                for (int i = 0; i < kHangSamples; ++i) {
                    lo = std::min(lo, pcRing[i]);
                    hi = std::max(hi, pcRing[i]);
                }
                if (hi - lo <= 0x2000) {   // wide window: catch big freeze loops too
                    // Confined. Dump once immediately, then keep re-dumping
                    // every ~3 s while STILL confined — a transient slow chunk
                    // dumps once then escapes; a permanent freeze keeps
                    // printing "STILL FROZEN".
                    if (!hangDumped) {
                        hangDumped = true; framesConfined = 0;
                        dumpHang(lo, hi, /*repeat=*/false);
                    } else if (++framesConfined >= kHangSamples) {
                        framesConfined = 0;
                        dumpHang(lo, hi, /*repeat=*/true);
                    }
                } else {
                    hangDumped = false;   // PC escaped — re-arm for next freeze
                    framesConfined = 0;
                }
            }
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
#endif // !__EMSCRIPTEN__

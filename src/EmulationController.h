// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Drives the M6502 + Memory in a worker thread so the UI can render at
// 60 Hz without stalling the simulation. Single thread, single mutex; the
// UI thread takes the mutex briefly each frame to render the screen.

#ifndef POM2_EMULATION_CONTROLLER_H
#define POM2_EMULATION_CONTROLLER_H

#include "AudioDevice.h"
#include "CassetteDevice.h"
#include "Disk35Image.h"
#include "FloppySoundDevice.h"
#include "IWMDevice.h"
#include "M6502.h"
#include "Memory.h"
#include "NoSlotClock.h"
#include "RewindBuffer.h"
#include "SmartPortHub.h"
#include "Sony35Drive.h"
#include "SpeakerDevice.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class EmulationController
{
public:
    enum class Mode { Stopped, Running, Step };

    EmulationController();
    ~EmulationController();

    Memory&         memory()   { return mem; }
    M6502&          cpu()      { return processor; }

    /// Continuous state-recording ring buffer behind the rewind feature.
    /// Disabled by default (zero overhead); enable via
    /// rewind().setEnabled(true). The worker thread captures one frame at
    /// each 60 Hz boundary while enabled — see workerLoop().
    pom2::RewindBuffer& rewind() { return rewind_; }
    CassetteDevice&    cassette()    { return *tape; }
    SpeakerDevice&     speaker()     { return *spk; }
    /// 5.25" Disk II mechanical sounds (head step / motor / click).
    /// DiskIICard plug routes here.
    FloppySoundDevice& floppySound525() { return *floppy525; }
    /// 3.5" Sony / SmartPort mechanical sounds. Sony35Drive (//c+ on-
    /// board) and SmartPortCard (Liron-class slot card) route here.
    FloppySoundDevice& floppySound35()  { return *floppy35; }
    /// Legacy single-instance accessor — alias for the 5.25" device, kept
    /// only for any out-of-tree caller. Internal call sites should pick
    /// floppySound525()/floppySound35() explicitly.
    FloppySoundDevice& floppySound() { return *floppy525; }
    AudioDevice&       audio()       { return *audioDev; }
    pom2::IWMDevice&   iwm()         { return *iwmDev; }
    pom2::SmartPortHub& smartPortHub() { return *hub; }
    /// Dallas DS1216E "SmartWatch" — lives at controller scope so its
    /// (battery-backed on real hardware) state machine survives profile
    /// switches and CPU resets.
    pom2::NoSlotClock&  noSlotClock() { return *noSlotClock_; }
    pom2::Sony35Drive&  sony35Internal() { return *drive35Int; }
    pom2::Sony35Drive&  sony35External() { return *drive35Ext; }
    pom2::Disk35Image&  disk35Internal()  { return *image35Int; }
    pom2::Disk35Image&  disk35External()  { return *image35Ext; }

    /// Mount an 800K Sony 3.5" image into drive `idx` (0 = internal,
    /// 1 = external). Takes the state mutex while swapping the
    /// `Disk35Image` payload and notifying the Sony35Drive's disk-
    /// change flip-flop. Returns true on success; on failure the
    /// drive is left empty and the image's `lastError()` carries the
    /// reason.
    bool mount35(int idx, const std::string& path);

    /// Unmount whatever is in 3.5" drive `idx` (0/1). No-op when empty.
    void eject35(int idx);

    // ─── Cassette transport (forwarded to CassetteDevice under stateMtx) ──
    bool loadTape (const std::string& path);
    bool saveTape (const std::string& path);
    void playTape();
    void stopTape();
    void pauseTape(bool paused);
    void rewindTape();
    void ejectTape();
    void clearTapeCapture();
    void armRecording();   // takes stateMutex — safe to call off the CPU thread
    void seekTapeRelative(double deltaSeconds);
    void setCassetteVolume(float v);

    void start();

    /// Single-threaded tick path — execute ONE frame's worth of work
    /// (cyclesPerFrame for Running, drain stepsPending for Step,
    /// no-op for Stopped) and return. Designed for hosts that can't
    /// run a CPU worker thread (browser WASM without SharedArrayBuffer
    /// / pthreads); call once per render frame. Threaded hosts ignore
    /// this entirely — the `workerLoop` spawned by `start()` covers
    /// the same logic.
    void tickFrame();

    /// Park the worker: sets Mode::Stopped, wakes it, then blocks UNTIL the
    /// worker has actually parked at the Stopped idle wait — a hard
    /// guarantee (unbounded wait with a periodic warn log), because after
    /// stop() returns callers (applyProfile / restartEmulationFromSettings /
    /// shutdown) rebuild ROMs/SlotBus outside stateMutex(); a best-effort
    /// timeout returning early would hand them a use-after-free. The worker
    /// re-checks the mode between its 4096-cycle chunks and the per-frame
    /// budget is capped (CLI/AI clamp at 2 M cycles), so parking is prompt
    /// in practice.
    ///
    /// MUST NOT be called while holding stateMutex(): the worker needs that
    /// lock to finish its current chunk before it can park — a violation
    /// now DEADLOCKS (loudly, with the warn log) instead of silently
    /// degrading to a race.
    void stop();

    // Reset API — POM2 exposes 4 verbs. The MAME equivalents are only 2
    // (per Agent F audit, gap F-1-4): `machine_start` runs once at
    // power-on (RAM init pattern, region select) and `machine_reset`
    // (II/II+) / `reset_w` (IIe/IIc/IIc+) handle every subsequent reset.
    // POM2's split is:
    //
    //   softReset()    → MAME `reset_w(true)→reset_w(false)` sequence.
    //                    On IIe-class wipes the MMU/IOU/LC list; on
    //                    II/II+ only clears kbd strobe + cnxx tracker
    //                    (per `resetSoftSwitchesWarm`). A/X/Y/RAM/zp
    //                    all survive. SP decremented by 3 (Theme 7).
    //
    //   hardReset()    → Same MAME path as softReset but the CPU also
    //                    zeros A/X/Y. POM2-only convention to give the
    //                    user a "deterministic CPU state" without a
    //                    full RAM wipe. RAM contents preserved.
    //
    //   coldBoot()     → MAME `machine_start` + `machine_reset` combo:
    //                    wipes user RAM ($0000-$BFFF + LC + aux) with
    //                    the 00/FF pattern, then runs the full soft-
    //                    switch reset. The only path that wipes RAM.
    //
    //   bootFromSlot() → Synthetic shortcut: coldBoot + force PC=$Cn00
    //                    after validating the slot has the autostart
    //                    signature ($Cn01=$20, $Cn03=$00, $Cn05=$03,
    //                    $Cn07=$3C — Apple II Ref Manual Appx C). On
    //                    signature mismatch, falls back to coldBoot
    //                    so the F8 autostart firmware can scan slots
    //                    naturally (Theme 8).
    void hardReset();
    void softReset();
    void coldBoot();
    void bootFromSlot(int slot);
    void requestStep(int n = 1);   // queue n single-instruction steps

    void setMode(Mode m);
    Mode getMode() const { return mode.load(); }

    // ─── Rewind transport (UI-facing) ────────────────────────────────────
    // Coordinate the rewind ring buffer with the worker thread. While
    // "scrubbing", the worker is parked (Stopped) so the UI can freely
    // restore historical frames without the in-flight frame overrunning
    // them. All of these take stateMutex internally — call from the UI
    // thread, not the worker.

    /// Park the worker so historical frames can be restored, then report
    /// whether there is anything to scrub (rewind enabled + ≥ 1 frame).
    bool   rewindBeginScrub();
    /// Restore frame `index` (clamped to the ring). Caller must have begun
    /// scrubbing. Returns the clamped index, or RewindBuffer::kNoFrame if
    /// the ring is empty.
    size_t rewindSeek(size_t index);
    /// Restore the newest frame whose cycle stamp is <= `cycle`.
    size_t rewindSeekToCycle(uint64_t cycle);
    /// Leave scrub: discard the abandoned future after `index` and resume
    /// live execution from there.
    void   rewindEndAndResume(size_t index);
    /// Leave scrub but stay paused at the current frame (keeps the ring).
    void   rewindEndPaused();
    /// True once the worker has parked at the Stopped idle wait (test hook).
    bool   rewindIsParked() const { return workerParked_.load(); }

    // 6502 cycles per ImGui frame (CPU-pacing budget). Default = ~17 045
    // cycles/frame = 1.0227 MHz emulated. Setting it higher than the real
    // clock turbo-runs the CPU; UI uses this for the "MAX" button.
    void setCyclesPerFrame(int n) { cyclesPerFrame.store(n); }
    int  getCyclesPerFrame() const { return cyclesPerFrame.load(); }

    // Machine video standard (NTSC 60 Hz / PAL 50 Hz). Sets the worker's
    // frame-pacing interval and propagates the 262/312-line geometry to
    // Memory (for beam-racing). The CPU budget per frame (cyclesPerFrame) is
    // set separately from the active profile's defaultCyclesPerFrame, so the
    // effective clock = cyclesPerFrame × refreshHz works out to ~1.0227 MHz
    // (NTSC) / ~1.0156 MHz (PAL).
    void          setVideoStandard(VideoStandard s);
    VideoStandard getVideoStandard() const { return videoStandard_.load(); }

    // Block for up to `timeoutMs` until the CPU is paused at an
    // instruction boundary. Cheap: the worker holds `stateMutex` only
    // while running a slice, releases it on every iteration.
    std::mutex& stateMutex() { return stateMtx; }

private:
    Memory                          mem;
    M6502                           processor;
    std::unique_ptr<CassetteDevice>    tape;
    std::unique_ptr<SpeakerDevice>     spk;
    std::unique_ptr<FloppySoundDevice> floppy525;
    std::unique_ptr<FloppySoundDevice> floppy35;
    std::unique_ptr<AudioDevice>       audioDev;
    std::unique_ptr<pom2::IWMDevice>    iwmDev;
    std::unique_ptr<pom2::Disk35Image>  image35Int;
    std::unique_ptr<pom2::Disk35Image>  image35Ext;
    std::unique_ptr<pom2::Sony35Drive>  drive35Int;
    std::unique_ptr<pom2::Sony35Drive>  drive35Ext;
    std::unique_ptr<pom2::SmartPortHub> hub;
    std::unique_ptr<pom2::NoSlotClock>  noSlotClock_;

    pom2::RewindBuffer rewind_;

    std::atomic<Mode> mode{Mode::Stopped};
    std::atomic<int>  cyclesPerFrame{17045};
    // Worker frame-pacing interval (µs) and the active video standard. PAL
    // paces at 50 Hz (20000 µs), NTSC at 60 Hz (~16667 µs).
    std::atomic<int>  frameIntervalUs{1'000'000 / 60};
    std::atomic<VideoStandard> videoStandard_{VideoStandard::NTSC};
    std::atomic<int>  stepsPending{0};   // queued single-step count (Step mode)
    std::atomic<bool> exitRequested{false};
    // True while the worker is idling in the Stopped CV wait. The rewind
    // transport and stop() wait on this so a UI-thread restore / profile
    // rebuild can't be overrun by an in-flight Running frame. The Running
    // branch re-checks `mode` between 4096-cycle chunks, so the worker
    // parks within ~one chunk of a Stopped request.
    std::atomic<bool> workerParked_{false};

    std::mutex              stateMtx;
    std::condition_variable wakeCv;
    std::thread             worker;

    /// One CPU budget slice under stateMutex: normally M6502::run, but
    /// while a slot card claims DMA bus mastery (SoftCard Z80 — see
    /// SlotPeripheral::dmaActive) the slice is handed to the card's
    /// processor instead. The 6502 yields mid-chunk on the hand-over
    /// (the card calls M6502::stop() from its toggle write), so the
    /// swap takes effect at the next instruction boundary, not the next
    /// 4096-cycle chunk. Budget + return value stay in 6502 cycles in
    /// both branches (the card converts its own clock — emuCycles never
    /// leaves the 6502 domain).
    int  runCpuSlice(int chunk);
    /// One single-instruction step of the current bus master: the DMA
    /// claimant's processor when a card owns the bus (SoftCard Z80),
    /// else the 6502. All Step verbs (debugger, CLI --step, AI /step)
    /// route through this so stepping can't run parked-6502 code that a
    /// DMA-halted CPU would never execute.
    void stepBusMaster();
    void workerLoop();
    void waitUntilParked();      // block (bounded) until workerParked_ is set
    void flushAudioForRewind();  // silence the speaker after a time jump
};

#endif // POM2_EMULATION_CONTROLLER_H

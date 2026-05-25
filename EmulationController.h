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
    void seekTapeRelative(double deltaSeconds);
    void setCassetteVolume(float v);

    void start();
    /// Request the worker to park (sets Mode::Stopped + wakes it). NOTE: this
    /// does NOT block until the worker actually stops — it may still be mid-step
    /// for up to one budget chunk. Callers that need exclusive access to CPU /
    /// Memory after stop() (e.g. applyProfile's card teardown) MUST take
    /// stateMutex(): the worker only touches CPU/Memory under that lock, so the
    /// lock — not stop() — is what serialises against an in-flight step.
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
    void requestStep();        // single-instruction step

    void setMode(Mode m);
    Mode getMode() const { return mode.load(); }

    // 6502 cycles per ImGui frame (CPU-pacing budget). Default = ~17 045
    // cycles/frame = 1.0227 MHz emulated. Setting it higher than the real
    // clock turbo-runs the CPU; UI uses this for the "MAX" button.
    void setCyclesPerFrame(int n) { cyclesPerFrame.store(n); }
    int  getCyclesPerFrame() const { return cyclesPerFrame.load(); }

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

    std::atomic<Mode> mode{Mode::Stopped};
    std::atomic<int>  cyclesPerFrame{17045};
    std::atomic<bool> stepRequested{false};
    std::atomic<bool> exitRequested{false};

    std::mutex              stateMtx;
    std::condition_variable wakeCv;
    std::thread             worker;

    void workerLoop();
};

#endif // POM2_EMULATION_CONTROLLER_H

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
#include "M6502.h"
#include "Memory.h"
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
    CassetteDevice& cassette() { return *tape; }
    SpeakerDevice&  speaker()  { return *spk; }
    AudioDevice&    audio()    { return *audioDev; }

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
    void stop();
    void hardReset();          // re-fetches reset vector and clears soft switches
    void coldBoot();           // wipes user RAM ($0000-$BFFF) then hardReset()
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
    std::unique_ptr<CassetteDevice> tape;
    std::unique_ptr<SpeakerDevice>  spk;
    std::unique_ptr<AudioDevice>    audioDev;

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

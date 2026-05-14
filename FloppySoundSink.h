// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Header-only abstract interface so DiskIICard (and any other floppy
// peripheral) can notify a mechanical-sound consumer without dragging
// the miniaudio-based FloppySoundDevice TU into every test/headless
// build. Concrete implementation: FloppySoundDevice.

#ifndef POM2_FLOPPY_SOUND_SINK_H
#define POM2_FLOPPY_SOUND_SINK_H

class FloppySoundSink
{
public:
    virtual ~FloppySoundSink() = default;
    /// Motor state changed. `withDisk`=true picks the loaded vs empty
    /// spin sample pair.
    virtual void motor(bool on, bool withDisk) = 0;
    /// Head moved one stepper pulse. `newTrack` is the destination track
    /// (sound consumer derives cadence from inter-call timing).
    virtual void step(int newTrack) = 0;
    /// One-shot mechanical click — used for disk insert / eject.
    virtual void click() = 0;
};

#endif // POM2_FLOPPY_SOUND_SINK_H

// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// PrinterSoundSink — header-only abstract interface so `ImageWriter` can
// report mechanical events without dragging the miniaudio-based
// `PrinterSoundDevice` translation unit into every headless build and test.
// Same pattern, and the same reason, as `FloppySoundSink`.
//
// ── Why there is no `emuCycles` here, unlike the floppy ───────────────────
//
// `FloppySoundSink::step` carries an emulated-cycle stamp because the GUEST
// drives the stepper directly: under POM2's ~60x disk turbo a whole phase
// sweep lands inside one audio buffer, so wall-clock gaps collapse to zero and
// the cadence classifier goes deaf.
//
// The printer is not like that. `ImageWriter` consumes its input queue on its
// OWN wall-clock pacing model (`tick(double dt)`, at the head's cps), so the
// guest can fire a job in at any speed and the head still moves at 180 cps.
// These events are therefore already in real time when they are emitted, and
// stamping them would add nothing. (The printer plan's § 9 claimed otherwise;
// it was wrong, and this is the correction.)

#ifndef POM2_PRINTER_SOUND_SINK_H
#define POM2_PRINTER_SOUND_SINK_H

class PrinterSoundSink
{
public:
    virtual ~PrinterSoundSink() = default;

    /// The head fired a column of pins. `pins` is how many wires struck
    /// (1-9), which is what makes a dense graphic louder than a full stop.
    /// Called once per printed character and once per bit-image column, so
    /// at speed this arrives in the low thousands per second — the consumer
    /// is expected to turn a RATE into a buzz rather than trigger a click
    /// per call.
    virtual void strike(int pins) = 0;

    /// The carriage swept back to the left margin. `inches` is how far it
    /// travelled, which sets how long the sweep is heard for.
    virtual void carriageReturn(double inches) = 0;

    /// The platen advanced (or reversed) by `inches`. Sign is ignored by the
    /// sound — a reverse feed makes the same noise.
    virtual void paperFeed(double inches) = 0;

    /// Power switched on or off, for the relay/idle hum.
    virtual void power(bool on) = 0;
};

#endif // POM2_PRINTER_SOUND_SINK_H

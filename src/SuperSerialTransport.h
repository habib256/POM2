// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Host-transport seam for the Super Serial Card.  The 6551/card model owns
// guest-visible state and byte queues; runtime implementations own sockets,
// worker threads and wall-clock pacing.

#ifndef POM2_SUPER_SERIAL_TRANSPORT_H
#define POM2_SUPER_SERIAL_TRANSPORT_H

#include <cstdint>

namespace pom2 {

class SuperSerialTransport
{
public:
    virtual ~SuperSerialTransport() = default;

    virtual bool start(uint16_t port) = 0;
    virtual void stop() = 0;

    virtual bool isListening() const = 0;
    virtual uint16_t port() const = 0;

    /// The card calls this after a control-register write or snapshot restore.
    /// A wall-clock transport must discard any credit accumulated at the old
    /// baud rate before it drains another guest byte.
    virtual void resetPacing() = 0;
};

} // namespace pom2

#endif // POM2_SUPER_SERIAL_TRANSPORT_H

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// SuperSerialTransport — the host side of the Super Serial Card's bridge.
//
// The card owns the ACIA registers, the RX/TX rings, the telnet parser and
// the emulated line rate. A transport owns whatever carries bytes to and from
// a peer — a loopback TCP listener and its worker thread in the production
// case — and reaches the card only through the hooks on SuperSerialCard
// marked "Transport hooks".
//
// The split exists so the card can be driven with no socket and no thread:
// a fake transport calls the same hooks directly.

#ifndef POM2_SUPER_SERIAL_TRANSPORT_H
#define POM2_SUPER_SERIAL_TRANSPORT_H

#include <cstdint>

namespace pom2 {

class SuperSerialTransport
{
public:
    virtual ~SuperSerialTransport() = default;

    /// Begin carrying bytes on `port`. Returns false when the endpoint could
    /// not be acquired (a bind failure, say); the card stays usable, it just
    /// has nothing on the far end.
    virtual bool start(uint16_t port) = 0;

    /// Stop, and do not return until the transport's own threads are joined.
    /// Must be safe to call when nothing is running.
    virtual void stop() = 0;

    virtual bool isListening() const = 0;
    virtual uint16_t port() const = 0;
};

} // namespace pom2

#endif // POM2_SUPER_SERIAL_TRANSPORT_H

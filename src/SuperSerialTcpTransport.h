// POM2 Apple II Emulator
// Copyright (C) 2026
//
// The production Super Serial transport: a loopback TCP listener and its
// worker thread. The factory is kept out of the card header on purpose — the
// card must not acquire a socket or thread dependency just by being included.

#ifndef POM2_SUPER_SERIAL_TCP_TRANSPORT_H
#define POM2_SUPER_SERIAL_TCP_TRANSPORT_H

#include <memory>

class SuperSerialCard;

namespace pom2 {

class SuperSerialTransport;

std::unique_ptr<SuperSerialTransport>
makeSuperSerialTcpTransport(SuperSerialCard& card, int slot);

} // namespace pom2

#endif // POM2_SUPER_SERIAL_TCP_TRANSPORT_H

// POM2 Apple II Emulator
// Copyright (C) 2026

#ifndef POM2_SUPER_SERIAL_TCP_TRANSPORT_H
#define POM2_SUPER_SERIAL_TCP_TRANSPORT_H

#include <memory>

class SuperSerialCard;

namespace pom2 {

class SuperSerialTransport;

/// Runtime-owned loopback TCP/telnet adapter.  Keeping the factory out of the
/// card header prevents devices from acquiring socket or thread dependencies.
std::unique_ptr<SuperSerialTransport>
makeSuperSerialTcpTransport(SuperSerialCard& card);

} // namespace pom2

#endif // POM2_SUPER_SERIAL_TCP_TRANSPORT_H

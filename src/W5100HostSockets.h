// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#ifndef POM2_W5100_HOST_SOCKETS_H
#define POM2_W5100_HOST_SOCKETS_H

#include "W5100Socket.h"

#include <memory>

namespace pom2 {

/// BSD/Winsock implementation of the W5100 device-side socket factory.
std::unique_ptr<W5100SocketFactory> makeW5100HostSocketFactory();

} // namespace pom2

#endif // POM2_W5100_HOST_SOCKETS_H

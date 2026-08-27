// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Production W5100 socket factory: the real BSD/Winsock implementation behind
// the W5100Socket interface.
//
// Every hardening step the inline code carried is preserved here — SIGPIPE
// suppression, non-blocking mode, the Windows UDP connection-reset ioctl and
// the MSG_NOSIGNAL send. They are not incidental: see the comments at each
// site in W5100HostSockets.cpp for the failure each one prevents.

#ifndef POM2_W5100_HOST_SOCKETS_H
#define POM2_W5100_HOST_SOCKETS_H

#include "W5100Socket.h"

#include <memory>

namespace pom2 {

/// The factory W5100Device uses when nothing is injected. Tests substitute
/// their own so device behaviour can be driven without opening a host socket.
std::unique_ptr<W5100SocketFactory> makeHostW5100SocketFactory();

} // namespace pom2

#endif // POM2_W5100_HOST_SOCKETS_H

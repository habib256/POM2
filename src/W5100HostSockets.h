// POM2 Apple II Emulator
// Copyright (C) 2026 VERHILLE Arnaud
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.

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

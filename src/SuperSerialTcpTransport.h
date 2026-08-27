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

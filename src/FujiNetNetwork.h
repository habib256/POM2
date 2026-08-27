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

// FujiNetNetwork — the `N:` device, as the card sees it.
//
// FujiNetCard serves the peer's NETWORK unit locally when the built-in
// network is enabled (see FujiNetCard::setBuiltInNetwork for why that exists
// at all). The card's side of that is six calls: open a devicespec, close it,
// ask whether it is open, read the 4-byte status the firmware defines, and
// pull decoded bytes out.
//
// What it deliberately does NOT know is that the implementation resolves a
// hostname and speaks HTTP over a host socket. A card owning a socket is the
// thing the layer split exists to prevent, so the socket work lives in
// FujiNetNetDevice at RUNTIME and reaches the card through this interface.
//
// It also makes the `N:` path testable without a network: a fake returning
// canned bytes exercises every guest-visible path in the card.

#ifndef POM2_FUJINET_NETWORK_H
#define POM2_FUJINET_NETWORK_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace pom2 {

/// `N:` command bytes, verbatim from the firmware's fujiCommandID.h.
///
/// These are wire vocabulary, not an implementation detail: the CARD decodes
/// them out of the guest's SmartPort control list and dispatches on them, so
/// they belong with the interface both sides speak.
enum : uint8_t {
    kNetOpen     = 0x4F,   ///< 'O'
    kNetClose    = 0x43,   ///< 'C'
    kNetRead     = 0x52,   ///< 'R'
    kNetStatus   = 0x53,   ///< 'S'
    kNetWrite    = 0x57,   ///< 'W'
    kNetGetError = 0x45,   ///< 'E'
};

/// `N:` STATUS error bytes, as FujiNetNetDevice already reports them.
/// 1 = success, 144 = general failure, 170 = not found (the guest reads that
/// last one as "no such host").
constexpr uint8_t kNetErrSuccess      = 1;
constexpr uint8_t kNetErrGeneral      = 144;
constexpr uint8_t kNetErrFileNotFound = 170;

class FujiNetNetwork
{
public:
    virtual ~FujiNetNetwork() = default;

    /// Open a devicespec, e.g. "N:HTTP://THEOLDNET.COM/". False on failure;
    /// `status()` then carries the error code the guest reads back.
    virtual bool open(const std::string& devicespec) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;

    /// The firmware's 4-byte STATUS reply: bytes-waiting (LE 16), connected,
    /// error. Guest code sizes its buffer from that count, which the firmware
    /// caps at 512 — implementations must cap it too.
    virtual void status(uint8_t out[4]) const = 0;

    /// Bytes still buffered, and a read of at most that many.
    virtual std::size_t available() const = 0;
    virtual std::size_t read(uint8_t* dst, std::size_t n) = 0;
};

/// A `N:` device that is not there. Every open fails, nothing is ever
/// readable. Used when the built-in network is not wired — and by tests, so a
/// card can be built with no capacity to reach the network at all.
class NullFujiNetNetwork final : public FujiNetNetwork
{
public:
    bool open(const std::string&) override { return false; }
    void close() override {}
    bool isOpen() const override { return false; }
    void status(uint8_t out[4]) const override
    { out[0] = 0; out[1] = 0; out[2] = 0; out[3] = kNetErrGeneral; }
    std::size_t available() const override { return 0; }
    std::size_t read(uint8_t*, std::size_t) override { return 0; }
};

} // namespace pom2

#endif // POM2_FUJINET_NETWORK_H

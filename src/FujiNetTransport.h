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

// FujiNetTransport — how the HOST drives the FujiNet link, as opposed to what
// the CARD sends over it.
//
// FujiNetLink is the protocol: SmartPort commands, issued by FujiNetCard on
// the CPU thread. This is the other half — arming a transport, starting and
// stopping the worker, and reading back what it has been doing. Nothing here
// is reachable from the emulated machine; it exists for the config panel, the
// settings host and the CLI.
//
// The two are split because they have different owners and different layers.
// The card is a DEVICE and must not know that "the peer" is a socket, a USB
// CDC device or a helper process — that knowledge (threads, framing, sockets)
// lives in SpOverSlipLink at RUNTIME. Holding the concrete link by value was
// what forced the card itself up into RUNTIME, against the rule that a card
// may not own a thread.
//
// Mode/Stats/kDefaultTimeoutMs live here rather than on the implementation
// because the panel renders them and the settings host persists them: they
// are part of the contract, not an implementation detail.

#ifndef POM2_FUJINET_TRANSPORT_H
#define POM2_FUJINET_TRANSPORT_H

#include <cstdint>
#include <string>

namespace pom2 {

class FujiNetTransport
{
public:
    /// Default round-trip budget. Loopback answers in microseconds and a real
    /// board over USB in single-digit milliseconds, so this is ~50x headroom
    /// for the normal case while keeping a dead peer to a quarter-second
    /// stall — one dropped frame, once, instead of a full second.
    static constexpr int kDefaultTimeoutMs = 250;

    enum class Mode { Off, Tcp, Serial };

    struct Stats {
        uint64_t calls    = 0;
        uint64_t timeouts = 0;
        uint64_t stale    = 0;   ///< responses discarded on sequence mismatch
        uint64_t bytesOut = 0;
        uint64_t bytesIn  = 0;
    };

    virtual ~FujiNetTransport() = default;

    /// Which transport is armed, and its parameters. The panel edits both TCP
    /// and serial settings and the host persists both, so a user who switches
    /// TCP → serial → TCP does not lose their port: the setters record their
    /// parameters whether or not that transport is the active one.
    virtual void setTcpMode(uint16_t port) = 0;
    virtual void setSerialMode(std::string devicePath, int baud) = 0;
    virtual void setOff() = 0;
    virtual Mode mode() const = 0;

    virtual uint16_t           tcpPort()    const = 0;
    virtual const std::string& serialPath() const = 0;
    virtual int                serialBaud() const = 0;

    /// Start the worker. Returns false (with `errOut` filled) when the chosen
    /// transport could not even be armed — a TCP port already in use, say.
    virtual bool start(std::string& errOut) = 0;
    /// Stop the worker, join it, and release the peer. Safe to call twice.
    virtual void stop() = 0;
    virtual bool isRunning() const = 0;

    /// Human-readable transport summary and the last failure, for the panel.
    virtual std::string describe()  const = 0;
    virtual std::string lastError() const = 0;
    virtual Stats       stats()     const = 0;

    virtual int  timeoutMs() const = 0;
    virtual void setTimeoutMs(int ms) = 0;
};

/// An inert transport: nothing armed, nothing running, no counters.
///
/// This is what "a FujiNet card with no host transport" means, and it exists
/// so that state is representable without a null pointer. Tests use it to
/// build a card that cannot open a socket even by mistake; `start()` fails
/// with a message rather than pretending to succeed.
class NullFujiNetTransport final : public FujiNetTransport
{
public:
    void setTcpMode(uint16_t) override {}
    void setSerialMode(std::string, int) override {}
    void setOff() override {}
    Mode mode() const override { return Mode::Off; }

    uint16_t           tcpPort()    const override { return 0; }
    const std::string& serialPath() const override { return empty_; }
    int                serialBaud() const override { return 0; }

    bool start(std::string& errOut) override
    { errOut = "no transport is wired to this card"; return false; }
    void stop() override {}
    bool isRunning() const override { return false; }

    std::string describe()  const override { return "no transport"; }
    std::string lastError() const override { return {}; }
    Stats       stats()     const override { return {}; }

    int  timeoutMs() const override { return kDefaultTimeoutMs; }
    void setTimeoutMs(int) override {}

private:
    /// serialPath() returns a reference, so the empty answer needs storage.
    std::string empty_;
};

} // namespace pom2

#endif // POM2_FUJINET_TRANSPORT_H

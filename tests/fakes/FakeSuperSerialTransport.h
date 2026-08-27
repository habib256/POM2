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

#ifndef POM2_TEST_FAKE_SUPER_SERIAL_TRANSPORT_H
#define POM2_TEST_FAKE_SUPER_SERIAL_TRANSPORT_H

#include "SuperSerialCard.h"
#include "SuperSerialTransport.h"

#include <cstdint>
#include <vector>

namespace pom2::test {

/// A transport that carries no bytes anywhere: no socket, no listener, no
/// thread. It exists to let a test play the far end by hand — deliver an
/// inbound chunk, take whatever the guest queued — on the calling thread, so
/// the assertions are deterministic rather than racing a worker.
class FakeSuperSerialTransport final : public SuperSerialTransport
{
public:
    explicit FakeSuperSerialTransport(SuperSerialCard& card) : card_(card) {}

    bool start(uint16_t p) override
    {
        port_ = p;
        listening_ = true;
        ++startCount;
        return startSucceeds;
    }

    void stop() override
    {
        if (connected_) disconnect();
        listening_ = false;
        ++stopCount;
    }

    bool isListening() const override { return listening_; }
    uint16_t port() const override { return port_; }

    // ── the far end, driven by the test ──────────────────────────────────

    void connect()
    {
        connected_ = true;
        card_.onTransportConnected();
    }

    void disconnect()
    {
        connected_ = false;
        card_.onTransportDisconnected();
    }

    /// Deliver bytes as if they had arrived from a peer. `textMode` mirrors
    /// the production worker: raw mode skips filtering AND the keyboard sink.
    void deliver(std::vector<uint8_t> bytes, bool textMode)
    {
        if (bytes.empty()) return;
        size_t n = bytes.size();
        if (textMode) n = card_.processTransportTextRx(bytes.data(), n);
        card_.deliverTransportBytes(bytes.data(), n, textMode);
    }

    /// Take whatever the guest has queued, exactly as the worker does.
    std::vector<uint8_t> drain()
    {
        std::vector<uint8_t> out;
        lastDrainTaken = card_.drainTransportTx(out);
        return out;
    }

    bool startSucceeds = true;
    int  startCount = 0;
    int  stopCount = 0;
    std::size_t lastDrainTaken = 0;

private:
    SuperSerialCard& card_;
    uint16_t port_ = 0;
    bool listening_ = false;
    bool connected_ = false;
};

} // namespace pom2::test

#endif // POM2_TEST_FAKE_SUPER_SERIAL_TRANSPORT_H

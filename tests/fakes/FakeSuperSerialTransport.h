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

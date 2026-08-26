#ifndef POM2_TEST_FAKE_SUPER_SERIAL_TRANSPORT_H
#define POM2_TEST_FAKE_SUPER_SERIAL_TRANSPORT_H

#include "SuperSerialTransport.h"

#include <cstdint>
#include <functional>
#include <utility>

namespace pom2::test {

class FakeSuperSerialTransport final : public SuperSerialTransport
{
public:
    using ConnectionSink = std::function<void(bool)>;

    explicit FakeSuperSerialTransport(ConnectionSink sink = {})
        : connectionSink_(std::move(sink))
    {}

    bool start(uint16_t requestedPort) override
    {
        ++startCount;
        lastRequestedPort = requestedPort;
        port_ = requestedPort;
        listening_ = startResult;
        if (listening_ && connectionSink_) connectionSink_(true);
        return listening_;
    }

    void stop() override
    {
        ++stopCount;
        listening_ = false;
        if (connectionSink_) connectionSink_(false);
    }

    bool isListening() const override { return listening_; }
    uint16_t port() const override { return port_; }
    void resetPacing() override { ++pacingResetCount; }

    bool startResult = true;
    int startCount = 0;
    int stopCount = 0;
    int pacingResetCount = 0;
    uint16_t lastRequestedPort = 0;

private:
    ConnectionSink connectionSink_;
    bool listening_ = false;
    uint16_t port_ = 0;
};

} // namespace pom2::test

#endif // POM2_TEST_FAKE_SUPER_SERIAL_TRANSPORT_H

#ifndef POM2_TEST_FAKE_W5100_SOCKET_H
#define POM2_TEST_FAKE_W5100_SOCKET_H

#include "W5100Socket.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pom2::test {

class FakeW5100HostSocket final : public W5100HostSocket
{
public:
    struct Packet {
        W5100ReceiveResult result;
        std::vector<uint8_t> data;
    };

    W5100ConnectResult connect(uint32_t address, uint16_t port) override
    {
        ++connectCount;
        lastConnectAddress = address;
        lastConnectPort = port;
        return connectResult;
    }

    W5100ConnectResult pollConnect() override
    {
        ++pollConnectCount;
        return pollConnectResult;
    }

    W5100ReceiveResult receive(uint8_t* data, std::size_t capacity) override
    {
        ++receiveCount;
        if (packets.empty()) return {W5100IoStatus::WouldBlock, 0, 0, 0};
        Packet packet = std::move(packets.front());
        packets.pop_front();
        const std::size_t copied = std::min(capacity, packet.data.size());
        if (copied) std::memcpy(data, packet.data.data(), copied);
        packet.result.bytes = copied;
        return packet.result;
    }

    W5100SendResult send(const uint8_t* data, std::size_t size,
                         uint32_t address, uint16_t port) override
    {
        ++sendCount;
        lastSendAddress = address;
        lastSendPort = port;
        if (data && size) sentBytes.insert(sentBytes.end(), data, data + size);
        if (sendResult.status == W5100IoStatus::Ok && sendResult.bytes == 0)
            return {W5100IoStatus::Ok, size};
        return sendResult;
    }

    W5100ConnectResult connectResult = W5100ConnectResult::Connected;
    W5100ConnectResult pollConnectResult = W5100ConnectResult::Connected;
    W5100SendResult sendResult{W5100IoStatus::Ok, 0};
    std::deque<Packet> packets;
    std::vector<uint8_t> sentBytes;
    uint32_t lastConnectAddress = 0;
    uint16_t lastConnectPort = 0;
    uint32_t lastSendAddress = 0;
    uint16_t lastSendPort = 0;
    int connectCount = 0;
    int pollConnectCount = 0;
    int receiveCount = 0;
    int sendCount = 0;
};

class FakeW5100SocketFactory final : public W5100SocketFactory
{
public:
    std::unique_ptr<W5100HostSocket> open(W5100SocketKind kind) override
    {
        ++openCount;
        lastKind = kind;
        auto socket = std::make_unique<FakeW5100HostSocket>();
        socket->connectResult = nextConnectResult;
        socket->pollConnectResult = nextPollConnectResult;
        lastSocket = socket.get();
        return socket;
    }

    uint32_t resolveHostname(const std::string& name, int waitMs) override
    {
        ++resolveCount;
        lastHostname = name;
        lastResolveWaitMs = waitMs;
        return resolveResult;
    }

    void pollResolver() override { ++pollResolverCount; }

    void clearResolverCache() override { ++clearResolverCacheCount; }

    W5100ConnectResult nextConnectResult = W5100ConnectResult::Connected;
    W5100ConnectResult nextPollConnectResult = W5100ConnectResult::Connected;
    uint32_t resolveResult = 0;
    FakeW5100HostSocket* lastSocket = nullptr;
    W5100SocketKind lastKind = W5100SocketKind::Tcp;
    std::string lastHostname;
    int lastResolveWaitMs = 0;
    int openCount = 0;
    int resolveCount = 0;
    int pollResolverCount = 0;
    int clearResolverCacheCount = 0;
};

} // namespace pom2::test

#endif // POM2_TEST_FAKE_W5100_SOCKET_H

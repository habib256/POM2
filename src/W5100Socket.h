// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026
//
// Device-side host-I/O seam for the W5100 offload engine. It carries only
// protocol-neutral values and byte buffers: no BSD/Winsock handles, sockaddr,
// resolver threads or platform headers may cross into the device layer.

#ifndef POM2_W5100_SOCKET_H
#define POM2_W5100_SOCKET_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace pom2 {

enum class W5100SocketKind { Tcp, Udp };
enum class W5100ConnectResult { Connected, InProgress, Failed };
enum class W5100IoStatus { Ok, WouldBlock, Closed, Discarded, Failed };

struct W5100ReceiveResult {
    W5100IoStatus status = W5100IoStatus::Failed;
    std::size_t   bytes = 0;
    uint32_t      sourceAddress = 0;  // network byte order
    uint16_t      sourcePort = 0;     // network byte order
};

struct W5100SendResult {
    W5100IoStatus status = W5100IoStatus::Failed;
    std::size_t   bytes = 0;
};

class W5100HostSocket
{
public:
    virtual ~W5100HostSocket() = default;

    virtual W5100ConnectResult connect(uint32_t address,
                                       uint16_t port) = 0;
    virtual W5100ConnectResult pollConnect() = 0;
    virtual W5100ReceiveResult receive(uint8_t* data,
                                       std::size_t capacity) = 0;
    virtual W5100SendResult send(const uint8_t* data, std::size_t size,
                                 uint32_t address, uint16_t port) = 0;
};

class W5100SocketFactory
{
public:
    virtual ~W5100SocketFactory() = default;

    virtual std::unique_ptr<W5100HostSocket> open(W5100SocketKind kind) = 0;

    /// Resolve within `waitMs`. A timed-out lookup may complete into the
    /// factory cache later; pollResolver() publishes those late answers.
    virtual uint32_t resolveHostname(const std::string& name, int waitMs) = 0;
    virtual void pollResolver() = 0;
    virtual void clearResolverCache() = 0;
};

} // namespace pom2

#endif // POM2_W5100_SOCKET_H

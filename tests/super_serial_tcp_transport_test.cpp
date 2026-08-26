// End-to-end seam test: deterministic SSC device + runtime TCP adapter.

#include "SocketCompat.h"
#include "SuperSerialCard.h"
#include "SuperSerialTcpTransport.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

namespace {

template <typename Predicate>
bool waitFor(Predicate predicate,
             std::chrono::milliseconds timeout = std::chrono::seconds(3))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return predicate();
}

} // namespace

int main()
{
#if !POM2_HAS_SOCKETS
    std::printf("SKIP super_serial_tcp_transport (no host sockets)\n");
    return 0;
#else
    pom2::ensureSocketStack();

    SuperSerialCard card(2);
    card.setRawMode(true);
    card.setTransport(pom2::makeSuperSerialTcpTransport(card));

    assert(card.startListening(0));
    const uint16_t port = card.getPort();
    assert(port != 0); // port 0 asks the OS for an ephemeral listener

    const pom2::socket_t client = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(pom2::isValidSocket(client));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    assert(::connect(client, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0);
    assert(pom2::setNonBlocking(client));
    assert(waitFor([&] { return card.clientConnected(); }));

    const uint8_t inbound = 'R';
    assert(pom2::sendSocket(client, &inbound, 1) == 1);
    assert(waitFor([&] { return card.rxQueueDepth() == 1; }));
    assert(card.deviceSelectRead(0x8) == inbound);

    // DTR must be asserted before the 6551 accepts a transmit byte.
    card.deviceSelectWrite(0xA, 0x01);
    card.deviceSelectWrite(0x8, 'T');
    uint8_t outbound = 0;
    assert(waitFor([&] {
        return pom2::recvSocket(client, &outbound, 1) == 1;
    }));
    assert(outbound == 'T');

    pom2::closeHostSocketValue(client);
    card.stopListening();
    assert(!card.isListening());
    assert(!card.clientConnected());
    std::printf("OK super_serial_tcp_transport\n");
    return 0;
#endif
}

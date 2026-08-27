// POM2 — W5100 host-socket seam.
//
// Pins that W5100Device drives its host socket through the W5100Socket
// interface, so device behaviour can be exercised with NO host socket opened:
// no bind, no connect, no port, nothing for a sandboxed or parallel CI run to
// collide with.
//
// The two cases below are the ones that were previously unreachable without a
// real peer on the far end, and both are behaviours the chip is expected to
// get right rather than incidental plumbing.

#include "W5100Device.h"

#include "fakes/FakeW5100Socket.h"

#include <cassert>
#include <cstdio>
#include <memory>

namespace {

using namespace pom2;

constexpr uint16_t socketReg(int index, uint8_t offset)
{
    return static_cast<uint16_t>(kW5100S0Base + (index << 8) + offset);
}

// Drive the guest-visible register sequence for "open a TCP socket".
void openTcpSocket(W5100Device& device, int index)
{
    device.writeValueAt(socketReg(index, kW5100SnMr), kW5100SnMrTcp);
    device.writeValueAt(socketReg(index, kW5100SnCr), kW5100SnCrOpen);
}

void connectTo(W5100Device& device, int index, uint32_t ipBigEndian,
               uint16_t port)
{
    device.writeValueAt(socketReg(index, kW5100SnDipr0),
                        static_cast<uint8_t>(ipBigEndian >> 24));
    device.writeValueAt(socketReg(index, kW5100SnDipr0 + 1),
                        static_cast<uint8_t>(ipBigEndian >> 16));
    device.writeValueAt(socketReg(index, kW5100SnDipr0 + 2),
                        static_cast<uint8_t>(ipBigEndian >> 8));
    device.writeValueAt(socketReg(index, kW5100SnDipr0 + 3),
                        static_cast<uint8_t>(ipBigEndian));
    device.writeValueAt(socketReg(index, kW5100SnDport0),
                        static_cast<uint8_t>(port >> 8));
    device.writeValueAt(socketReg(index, kW5100SnDport1),
                        static_cast<uint8_t>(port));
    device.writeValueAt(socketReg(index, kW5100SnCr), kW5100SnCrConnect);
}

// ── Case 1: OPEN goes through the factory, and the kind follows SnMR ──────
void testOpenUsesInjectedFactory()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    assert(fake->openCount == 0);
    assert(!device.socketInfo(0).hasHostSocket);

    openTcpSocket(device, 0);

    // One host socket, of the kind the guest asked for in SnMR. Nothing was
    // opened on the real stack: with the inline ::socket() call this test
    // could not have existed without binding something.
    assert(fake->openCount == 1);
    assert(fake->lastKind == W5100SocketKind::Tcp);
    assert(device.socketInfo(0).hasHostSocket);

    // CLOSE releases the handle, which is what closes the host socket now —
    // the owning handle replaced a raw fd plus a manual closeHostSocket().
    device.writeValueAt(socketReg(0, kW5100SnCr), kW5100SnCrClose);
    assert(!device.socketInfo(0).hasHostSocket);

    std::printf("  open routes through the factory, kind follows SnMR: OK\n");
}

// ── Case 2: a non-blocking connect parks in SYN_SENT until it completes ───
//
// This is the case the inline implementation could only reach against a real
// unreachable host, timing-dependent. The guest polls SN_SR for $13 (SYN_SENT)
// and must see $17 (ESTABLISHED) only once the connection actually completes.
void testConnectInProgressParksInSynSent()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    fake->nextConnectResult = W5100ConnectResult::InProgress;
    fake->nextPollConnectResult = W5100ConnectResult::InProgress;
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    openTcpSocket(device, 0);
    connectTo(device, 0, 0x0A000001u, 8080);

    assert(fake->lastSocket != nullptr);
    assert(fake->lastSocket->connectCount == 1);
    // The destination reached the socket as the guest wrote it.
    assert(fake->lastSocket->lastConnectPort != 0);
    assert(device.socketInfo(0).status == kW5100SnSrSynSent);

    // Still pending: poll() must not promote it.
    device.poll();
    assert(device.socketInfo(0).status == kW5100SnSrSynSent);

    // Completed: the next poll promotes to ESTABLISHED.
    fake->lastSocket->pollConnectResult = W5100ConnectResult::Connected;
    device.poll();
    assert(device.socketInfo(0).status == kW5100SnSrEstablished);

    std::printf("  connect InProgress parks in SYN_SENT, then promotes: OK\n");
}

// ── Case 3: a refused connection returns the socket to CLOSED ─────────────
void testConnectRefusedClosesSocket()
{
    W5100Device device;
    auto factory = std::make_unique<test::FakeW5100SocketFactory>();
    auto* fake = factory.get();
    fake->nextConnectResult = W5100ConnectResult::InProgress;
    fake->nextPollConnectResult = W5100ConnectResult::Failed;
    device.setSocketFactory(std::move(factory));
    device.reset(true);

    openTcpSocket(device, 0);
    connectTo(device, 0, 0x0A000001u, 9);

    assert(device.socketInfo(0).status == kW5100SnSrSynSent);
    device.poll();
    // Refused / unreachable is CLOSED, which is what the guest polls SN_SR
    // for. Leaving it in SYN_SENT hangs every driver that waits on it.
    assert(device.socketInfo(0).status == kW5100SnSrClosed);
    assert(!device.socketInfo(0).hasHostSocket);

    std::printf("  refused connection returns to CLOSED: OK\n");
}

} // namespace

int main()
{
    std::printf("W5100 host-socket seam\n");
    testOpenUsesInjectedFactory();
    testConnectInProgressParksInSynSent();
    testConnectRefusedClosesSocket();
    std::printf("OK\n");
    return 0;
}

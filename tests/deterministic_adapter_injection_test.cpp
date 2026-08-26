#include "FujiNetCard.h"
#include "M6502.h"
#include "Memory.h"
#include "SuperSerialCard.h"
#include "W5100Device.h"
#include "fakes/FakeFujiNetLink.h"
#include "fakes/FakeSuperSerialTransport.h"
#include "fakes/FakeW5100Socket.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

void testSerialTransport()
{
    SuperSerialCard card(2);
    auto fake = std::make_unique<pom2::test::FakeSuperSerialTransport>(
        [&card](bool connected) { card.setTransportConnected(connected); });
    auto* observer = fake.get();
    card.setTransport(std::move(fake));

    assert(card.startListening(6502));
    assert(observer->startCount == 1);
    assert(observer->lastRequestedPort == 6502);
    assert(card.clientConnected());

    card.deviceSelectWrite(0x0B, 0x0E);
    assert(observer->pacingResetCount == 1);
    card.stopListening();
    assert(observer->stopCount >= 1);
    assert(!card.clientConnected());
}

void testFujiNetLink()
{
    Memory memory;
    memory.clearRam();
    memory.resetSoftSwitches();
    M6502 cpu(&memory);
    memory.setCpu(&cpu);
    cpu.hardReset();

    pom2::FujiNetCard card(7);
    card.setMemory(&memory);
    card.setCpu(&cpu);
    auto fake = std::make_unique<pom2::test::FakeFujiNetLink>();
    auto* observer = fake.get();
    fake->queueResponse({true, pom2::kSpOk, {0xF8, 0x34, 0x12, 0x00}});
    card.setLink(std::move(fake));

    // A ProDOS STATUS call is enough to prove the card talks only through
    // the injected link and maps its deterministic response back to X/Y.
    memory.memWrite(0x0042, 0x00);
    memory.memWrite(0x0043, 0x00); // drive 1 -> FujiNet unit 1
    card.deviceSelectWrite(0x02, pom2::FujiNetCard::kMagicProDOS);
    assert(observer->lastOperation ==
           pom2::test::FakeFujiNetLink::Operation::Status);
    assert(observer->lastUnit == 1);
    assert(cpu.getAccumulator() == pom2::kSpOk);
    assert(cpu.getXRegister() == 0x34);
    assert(cpu.getYRegister() == 0x12);

    card.onReset();
    assert(observer->guestResetCount == 1);
    const uint8_t snapshot[] = {'F', 'N', 'E', 'T', 0x01, 0x01};
    card.loadSnapshotState(snapshot, sizeof(snapshot));
    assert(observer->resyncCount == 1);
}

void testW5100SocketFactory()
{
    pom2::W5100Device device;
    auto fake = std::make_unique<pom2::test::FakeW5100SocketFactory>();
    auto* factory = fake.get();
    fake->nextConnectResult = pom2::W5100ConnectResult::InProgress;
    fake->nextPollConnectResult = pom2::W5100ConnectResult::Connected;
    device.setSocketFactory(std::move(fake));

    const uint16_t base = pom2::kW5100S0Base;
    device.writeValueAt(base + pom2::kW5100SnMr, pom2::kW5100SnMrTcp);
    device.writeValueAt(base + pom2::kW5100SnDipr0, 127);
    device.writeValueAt(base + pom2::kW5100SnDipr0 + 1, 0);
    device.writeValueAt(base + pom2::kW5100SnDipr0 + 2, 0);
    device.writeValueAt(base + pom2::kW5100SnDipr3, 1);
    device.writeValueAt(base + pom2::kW5100SnDport0, 0x19);
    device.writeValueAt(base + pom2::kW5100SnDport1, 0x66);

    device.writeValueAt(base + pom2::kW5100SnCr, pom2::kW5100SnCrOpen);
    assert(factory->openCount == 1);
    assert(factory->lastKind == pom2::W5100SocketKind::Tcp);
    assert(device.socketInfo(0).status == pom2::kW5100SnSrInit);

    device.writeValueAt(base + pom2::kW5100SnCr, pom2::kW5100SnCrConnect);
    assert(factory->lastSocket != nullptr);
    assert(factory->lastSocket->connectCount == 1);
    assert(device.socketInfo(0).status == pom2::kW5100SnSrSynSent);

    device.poll();
    assert(factory->pollResolverCount == 1);
    assert(factory->lastSocket->pollConnectCount == 1);
    assert(device.socketInfo(0).status == pom2::kW5100SnSrEstablished);
}

} // namespace

int main()
{
    testSerialTransport();
    testFujiNetLink();
    testW5100SocketFactory();
    std::puts("deterministic adapter injection: OK");
    return 0;
}

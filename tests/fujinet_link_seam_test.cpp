// POM2 — FujiNet command-surface seam.
//
// Pins that FujiNetCard talks to a FujiNetLink rather than to SP-over-SLIP
// directly, so card behaviour can be driven with canned SmartPort replies and
// NO socket, helper process or peer.
//
// Everything below was previously reachable only by standing up a real
// SP-over-SLIP peer on loopback: the card's printer-unit detection and its
// printer tap both depend on what the far end reports in its device
// enumeration, which a test had no way to control.

#include "FujiNetCard.h"

#include "fakes/FakeFujiNetLink.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace pom2;

test::FakeFujiNetLink& attachFake(FujiNetCard& card, test::FakeFujiNetLink& fake)
{
    card.setLinkForTesting(&fake);
    return fake;
}

// ── A peer with no printer: the tray must not claim one ──────────────────
void testNoPrinterUnit()
{
    FujiNetCard card(7);
    test::FakeFujiNetLink fake;
    fake.deviceList = {
        SpDevice{ 1, "DISK", kSpTypeHardDisk, 0, 65535 },
    };
    attachFake(card, fake);

    assert(!card.hasPrinterUnit());
    std::printf("  a peer with no printer reports none: OK\n");
}

// ── A peer that names a printer by DIB string ────────────────────────────
//
// SpDevice::isPrinter accepts EITHER the "PRINTER" DIB name or the SmartPort
// type byte, because real FujiNet builds have shipped both. Pin both halves:
// getting this wrong makes the paper tray show an unconnected printer while
// the guest happily prints.
void testPrinterDetectedByName()
{
    FujiNetCard card(7);
    test::FakeFujiNetLink fake;
    fake.deviceList = {
        SpDevice{ 1, "DISK",    kSpTypeHardDisk, 0, 65535 },
        SpDevice{ 2, "PRINTER", 0,               0, 0     },
    };
    attachFake(card, fake);

    assert(card.hasPrinterUnit());
    std::printf("  printer detected by DIB name: OK\n");
}

void testPrinterDetectedByType()
{
    FujiNetCard card(7);
    test::FakeFujiNetLink fake;
    fake.deviceList = {
        SpDevice{ 1, "SOMETHING", kSpTypePrinter, 0, 0 },
    };
    attachFake(card, fake);

    assert(card.hasPrinterUnit());
    std::printf("  printer detected by SmartPort type byte: OK\n");
}

// ── The enumeration is re-read, not cached at attach ─────────────────────
//
// A FujiNet re-enumerates when its config changes, and the card must follow.
// Caching the answer meant plugging a printer into a running FujiNet never
// reached the paper tray.
void testEnumerationIsLive()
{
    FujiNetCard card(7);
    test::FakeFujiNetLink fake;
    fake.deviceList = { SpDevice{ 1, "DISK", kSpTypeHardDisk, 0, 65535 } };
    attachFake(card, fake);

    assert(!card.hasPrinterUnit());

    fake.deviceList.push_back(SpDevice{ 2, "PRINTER", kSpTypePrinter, 0, 0 });
    assert(card.hasPrinterUnit());

    std::printf("  device enumeration is re-read, not cached: OK\n");
}

} // namespace

int main()
{
    std::printf("FujiNet command-surface seam\n");
    testNoPrinterUnit();
    testPrinterDetectedByName();
    testPrinterDetectedByType();
    testEnumerationIsLive();
    std::printf("OK\n");
    return 0;
}

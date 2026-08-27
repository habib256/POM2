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
//
// The card now takes its host side by INJECTION, so the seam is structural
// rather than a test hook: every card built here owns a fake link and the
// null transport/network, and has no capacity to open a socket at all. That
// is also what lets FujiNetCard be a DEVICE — a card may not own a thread.

#include "FujiNetCard.h"
#include "FujiNetNetwork.h"
#include "FujiNetTransport.h"

#include "fakes/FakeFujiNetLink.h"

#include <memory>

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

namespace {

using namespace pom2;

/// A card wired to a fake peer and to nothing else.
///
/// The fake is OWNED by the card (that is the production ownership shape), so
/// the rig keeps a raw pointer to go on steering it after construction — the
/// enumeration-is-live case depends on mutating the fake mid-test.
struct Rig {
    NullFujiNetTransport  transport;
    NullFujiNetNetwork*   network = nullptr;   // owned by the card
    test::FakeFujiNetLink* fake   = nullptr;   // owned by the card
    std::unique_ptr<FujiNetCard> card;

    explicit Rig(int slot = 7)
    {
        auto link = std::make_unique<test::FakeFujiNetLink>();
        fake = link.get();
        auto net = std::make_unique<NullFujiNetNetwork>();
        network = net.get();
        card = std::make_unique<FujiNetCard>(slot, std::move(link), transport,
                                             std::move(net));
    }
};

// ── A peer with no printer: the tray must not claim one ──────────────────
void testNoPrinterUnit()
{
    Rig rig;
    rig.fake->deviceList = {
        SpDevice{ 1, "DISK", kSpTypeHardDisk, 0, 65535 },
    };
    assert(!rig.card->hasPrinterUnit());
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
    Rig rig;
    rig.fake->deviceList = {
        SpDevice{ 1, "DISK",    kSpTypeHardDisk, 0, 65535 },
        SpDevice{ 2, "PRINTER", 0,               0, 0     },
    };
    assert(rig.card->hasPrinterUnit());
    std::printf("  printer detected by DIB name: OK\n");
}

void testPrinterDetectedByType()
{
    Rig rig;
    rig.fake->deviceList = {
        SpDevice{ 1, "SOMETHING", kSpTypePrinter, 0, 0 },
    };
    assert(rig.card->hasPrinterUnit());
    std::printf("  printer detected by SmartPort type byte: OK\n");
}

// ── The enumeration is re-read, not cached at attach ─────────────────────
//
// A FujiNet re-enumerates when its config changes, and the card must follow.
// Caching the answer meant plugging a printer into a running FujiNet never
// reached the paper tray.
void testEnumerationIsLive()
{
    Rig rig;
    rig.fake->deviceList = { SpDevice{ 1, "DISK", kSpTypeHardDisk, 0, 65535 } };
    assert(!rig.card->hasPrinterUnit());

    rig.fake->deviceList.push_back(SpDevice{ 2, "PRINTER", kSpTypePrinter, 0, 0 });
    assert(rig.card->hasPrinterUnit());

    std::printf("  device enumeration is re-read, not cached: OK\n");
}

// ── A card with no transport refuses, it does not pretend ────────────────
//
// The null transport is what "no host transport is wired" looks like without
// a null pointer. The distinction that matters is that start() FAILS with a
// message: a silent success would leave the panel showing a running link with
// no peer behind it, which is exactly the state that is hardest to diagnose.
void testNullTransportRefusesToStart()
{
    Rig rig;
    assert(!rig.card->transportLink().isRunning());
    assert(rig.card->transportLink().mode() == FujiNetTransport::Mode::Off);

    std::string err;
    assert(!rig.card->transportLink().start(err));
    assert(!err.empty());

    // And it stays off: a failed start must not leave the card half-armed.
    assert(!rig.card->transportLink().isRunning());
    std::printf("  a card with no transport refuses to start: OK\n");
}

} // namespace

int main()
{
    std::printf("FujiNet command-surface seam\n");
    testNoPrinterUnit();
    testPrinterDetectedByName();
    testPrinterDetectedByType();
    testEnumerationIsLive();
    testNullTransportRefusesToStart();
    std::printf("OK\n");
    return 0;
}

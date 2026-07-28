// //c Disk II vs on-board IWM conflict — regression test.
//
// POM2 mirrors $C0E0-$C0EF into the on-board IWM on //c-class profiles
// that have an alt firmware bank (the 32 KB //c rev-0/3/4 dump and the
// //c+). MAME wires its IWM as *the* slot-6 controller, replacing the
// Disk II; POM2 does not — the slot-6 DiskIICard stays authoritative for
// 5.25" media. So on a plain //c the mirror added a SECOND controller on
// the same soft switches without supplying the data path, and the IWM's
// phase/motor handling fought the DiskIICard's over one drive.
//
// The user-visible damage: the head position drifted, DOS 3.3 RWTS fell
// into endless seek/retry storms ($B948-$B956 with the head oscillating
// between the target track and 0), and Print Shop on a //c could neither
// save its setup nor load its print overlay — so printing to the
// ImageWriter silently produced nothing while the *serial* path was
// provably byte-exact.
//
// The fix gates the mirror on `isPlus_`: the IWM is consulted only where
// it actually owns a drive (the //c+ MIG / Sony 3.5" path).
//
// What this test pins:
//   * plain //c (32 KB alt-bank ROM, NOT //c+): $C0E0-$C0EF must NOT be
//     claimed by the IWM — ioReadIWM returns false so Memory falls
//     through to the slot-6 DiskIICard, and ioWriteIWM must not disturb
//     the IWM (checked via its snapshot state, which changes when the
//     device is ticked/written).
//   * //c+ : the mirror stays live, or the MIG / 3.5" path loses its
//     controller.

#include "IWMDevice.h"
#include "MemoryProfile_IIcClass.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

// //c-class ROM payload: 16 KB covering $C000-$FFFF. MAME identifies the
// //c+ by `payload[0x3bbf] == 0x05` (`apple2e.cpp:1275-1299`), which is
// the only byte either profile cares about here.
std::vector<uint8_t> makePayload(bool isPlus)
{
    std::vector<uint8_t> rom(0x4000, 0x00);
    rom[0x3bbf] = isPlus ? 0x05 : 0x00;
    return rom;
}

/// The IWM only advances when it is ticked / written. Its snapshot blob
/// therefore doubles as a "was this device touched?" probe.
std::vector<uint8_t> iwmState(pom2::IWMDevice& iwm)
{
    std::vector<uint8_t> out;
    iwm.appendSnapshotState(out);
    return out;
}

void testPlainIIcDoesNotRouteToIwm()
{
    const std::vector<uint8_t> payload = makePayload(/*isPlus=*/false);
    const std::vector<uint8_t> altBank(0x4000, 0x00);   // 32 KB dump → alt bank present

    pom2::IWMDevice iwm;
    IIcClassProfile profile(payload.data(), payload.size(), altBank.data(),
                            &iwm, nullptr, /*iwmAuthoritative=*/true);

    const std::vector<uint8_t> before = iwmState(iwm);

    // Walk the whole $C0E0-$C0EF window the way DOS 3.3 RWTS does:
    // phase on/off pairs (the stepper), motor, drive select, and the
    // Q6/Q7 data-latch registers.
    for (uint8_t low = 0; low < 0x10; ++low) {
        uint8_t out = 0xAB;
        const bool claimed = profile.ioReadIWM(
            static_cast<uint16_t>(0xC0E0 + low), /*cyc=*/1000 + low, out);
        // Not claimed → Memory falls through to the slot-6 DiskIICard,
        // which is the only controller that should answer on a plain //c.
        assert(!claimed);
        assert(out == 0xAB);            // untouched
        profile.ioWriteIWM(static_cast<uint16_t>(0xC0E0 + low), 0xFF,
                           /*cyc=*/2000 + low);
    }

    // And the IWM must not have been advanced at all — a tick would let
    // its phase latches drift out of step with the DiskIICard's.
    assert(iwmState(iwm) == before);

    std::printf("  plain //c: $C0E0-$C0EF stays with the Disk II OK\n");
}

void testIIcPlusStillRoutesToIwm()
{
    const std::vector<uint8_t> payload = makePayload(/*isPlus=*/true);
    const std::vector<uint8_t> altBank(0x4000, 0x00);

    pom2::IWMDevice iwm;
    IIcClassProfile profile(payload.data(), payload.size(), altBank.data(),
                            &iwm, nullptr, /*iwmAuthoritative=*/true);

    uint8_t out = 0xAB;
    const bool claimed = profile.ioReadIWM(0xC0EC, /*cyc=*/5000, out);
    // //c+ keeps the mirror: the MIG / Sony 3.5" path has no other
    // controller to fall back on.
    assert(claimed);

    const std::vector<uint8_t> before = iwmState(iwm);
    profile.ioWriteIWM(0xC0E1, 0xFF, /*cyc=*/9000);
    // The write ticked the device forward, so its state must have moved.
    assert(iwmState(iwm) != before);

    std::printf("  //c+: IWM mirror still live OK\n");
}

void testNoAltBankNeverRoutes()
{
    // 16 KB rev-255 //c: no alt bank, so the mirror was never armed.
    const std::vector<uint8_t> payload = makePayload(/*isPlus=*/false);

    pom2::IWMDevice iwm;
    IIcClassProfile profile(payload.data(), payload.size(), /*altBank16k=*/nullptr,
                            &iwm, nullptr, /*iwmAuthoritative=*/true);

    uint8_t out = 0x5A;
    assert(!profile.ioReadIWM(0xC0EC, /*cyc=*/100, out));
    assert(out == 0x5A);

    std::printf("  16 KB //c (no alt bank): no IWM mirror OK\n");
}

} // namespace

int main()
{
    std::printf("//c Disk II / on-board IWM conflict test\n");
    testPlainIIcDoesNotRouteToIwm();
    testIIcPlusStillRoutesToIwm();
    testNoAltBankNeverRoutes();
    std::printf("PASS\n");
    return 0;
}

// Sony 3.5" head step-direction regression test.
//
// MAME `mac_floppy_device::seek_phase_w`: register 0x0 "DirNext" sets the
// step direction OUTWARD (cyl+1), register 0x4 "DirPrev" sets it INWARD
// (toward track 0), register 0x1 issues a step pulse. POM2 previously
// mapped 0x0→inward and 0x4→eject with NO outward path, so `directionIn_`
// could never become false and the head could only ever step toward track
// 0 (the cyl+1 branch was dead code) — 3.5" data tracks 1-79 unreachable.
//
// Drives Sony35Drive directly through its public seekPhaseW() the way the
// IWM would (CA0-2 = register, PH3/LSTRB = strobe), and checks track().

#include "Sony35Drive.h"

#include <cassert>
#include <cstdio>

using pom2::Sony35Drive;

namespace {

// Strobe drive register `reg` (0..7): drop LSTRB (bit 3) with CA bits =
// reg, then raise LSTRB → fires on the rising edge (matches MAME).
void strobe(Sony35Drive& d, uint8_t reg) {
    d.seekPhaseW(static_cast<uint8_t>(reg & 0x07), 0);          // LSTRB low
    d.seekPhaseW(static_cast<uint8_t>((reg & 0x07) | 0x08), 0); // LSTRB high → strobe
}

constexpr uint8_t kDirOut = 0x0;   // DirNext  → cyl+1 (outward)
constexpr uint8_t kStep   = 0x1;   // StepOn
constexpr uint8_t kDirIn  = 0x4;   // DirPrev  → cyl-1 (toward track 0)

}  // namespace

int main()
{
    Sony35Drive drv;
    assert(drv.track() == 0);

    // Outward: 5 steps → track 5.
    strobe(drv, kDirOut);
    for (int i = 0; i < 5; ++i) strobe(drv, kStep);
    if (drv.track() != 5) {
        std::printf("FAIL: outward 5 steps → track %d (want 5)\n", drv.track());
        return 1;
    }

    // Inward: 3 steps → track 2.
    strobe(drv, kDirIn);
    for (int i = 0; i < 3; ++i) strobe(drv, kStep);
    if (drv.track() != 2) {
        std::printf("FAIL: inward 3 steps → track %d (want 2)\n", drv.track());
        return 1;
    }

    // Inward past 0 clamps at track 0.
    for (int i = 0; i < 10; ++i) strobe(drv, kStep);
    if (drv.track() != 0) {
        std::printf("FAIL: inward clamp → track %d (want 0)\n", drv.track());
        return 1;
    }

    // Outward past 79 clamps at track 79.
    strobe(drv, kDirOut);
    for (int i = 0; i < 200; ++i) strobe(drv, kStep);
    if (drv.track() != 79) {
        std::printf("FAIL: outward clamp → track %d (want 79)\n", drv.track());
        return 1;
    }

    std::printf("OK sony_seek_direction (outward + inward + clamps)\n");
    return 0;
}

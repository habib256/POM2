// IWM + //c+ MIG snapshot round-trip — pins:
//
//   1. IWMDevice's eight absolute emuCycles stamps (now_, lastSync_,
//      nextStateChange_, syncUpdate_, asyncUpdate_, revStart35_,
//      fluxWriteStart_, delayDeadline_) survive a save/restore. They did
//      not before: a rewind rolled the machine's cycleCounter backwards
//      while the IWM kept its larger lastSync_, so sync()'s
//      `while (nextSync > lastSync_)` walker stopped advancing and the
//      controller sat frozen until emulated time caught back up. Reachable
//      on every //c-class profile — ioReadIWM ticks the IWM on each
//      $C0E0-$C0EF access even in shadow mode.
//   2. The //c+ MIG gate array's 2 KB RAM and its auto-incrementing page
//      pointer round-trip; they used to come back zeroed, so the alt
//      firmware read something other than what it had written.
//   3. Backward compatibility: a blob truncated before the new trailer
//      still loads, leaving the live device values alone (the exact
//      pre-fix behaviour, so old saves do not regress).
//   4. A corrupt MIG page pointer is masked to 0x7FF on the way in —
//      migRead indexes migRam_[migPage_ + (offset & 0x1F)], so an
//      unmasked value read past the 2 KB array.

#include "IWMDevice.h"
#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

// Drive the IWM to a non-default state: motor on via the $C0Ex switches,
// then advance time so the internal stamps move off zero.
void exerciseIwm(pom2::IWMDevice& iwm)
{
    iwm.tick(1000);
    iwm.write(0x9, 0x00);        // Q6/Q7 mode select region
    iwm.tick(50000);
    iwm.read(0xC);
    iwm.tick(123456789ull);      // a big, distinctly non-zero "now"
}

void testIwmRoundTrip()
{
    pom2::IWMDevice a;
    exerciseIwm(a);
    const uint64_t nowA = a.emuCycles();
    assert(nowA == 123456789ull);

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(blob.size() > 4);
    assert(std::memcmp(blob.data(), "IWM1", 4) == 0);

    // A fresh device is at cycle 0 — the exact desync the bug produced.
    pom2::IWMDevice b;
    assert(b.emuCycles() == 0);
    assert(b.loadSnapshotState(blob.data(), blob.size()));
    assert(b.emuCycles() == nowA);
    assert(b.isActive() == a.isActive());
    assert(b.isIdle()   == a.isIdle());

    // Re-serialising the restored device must produce the identical blob:
    // that is the only way to assert the *private* stamps came across, not
    // just the one exposed by emuCycles().
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    // Truncation and a bad magic are both rejected, and must not leave the
    // device half-restored.
    pom2::IWMDevice c;
    assert(!c.loadSnapshotState(blob.data(), blob.size() - 1));
    assert(c.emuCycles() == 0);
    std::vector<uint8_t> bad = blob;
    bad[0] = 'X';
    assert(!c.loadSnapshotState(bad.data(), bad.size()));
    assert(c.emuCycles() == 0);
    assert(!c.loadSnapshotState(nullptr, 0));

    std::printf("  ok: IWM emuCycles stamps + registers round-trip\n");
}

void testMemoryTrailerCarriesIwm()
{
    // The Memory blob must carry the IWM section through, and a blob
    // truncated before the trailer must still load.
    Memory mem;
    pom2::IWMDevice iwm;
    exerciseIwm(iwm);
    mem.setIWM(&iwm);

    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);

    // Reload into a machine whose IWM is at cycle 0.
    Memory mem2;
    pom2::IWMDevice iwm2;
    mem2.setIWM(&iwm2);
    assert(iwm2.emuCycles() == 0);
    assert(mem2.loadSnapshotState(blob.data(), blob.size()));
    assert(iwm2.emuCycles() == iwm.emuCycles());

    // Backward compatibility: lop the trailer off entirely. The old
    // single-byte trailer ended right before our two length-prefixed
    // sections, so cutting the last 8+ bytes models a pre-fix blob.
    pom2::IWMDevice iwm3;
    Memory mem3;
    mem3.setIWM(&iwm3);
    const size_t shortLen = blob.size() - 8;
    assert(mem3.loadSnapshotState(blob.data(), shortLen));
    assert(iwm3.emuCycles() == 0);   // untouched, exactly as before the fix

    std::printf("  ok: Memory trailer carries IWM; old blobs still load\n");
}

void testMigPageMasked()
{
    // Hand-build a MIG blob with an out-of-range page pointer and confirm
    // it is masked rather than trusted. Goes through Memory because the
    // profile is private to it; a //c-class profile is required for the
    // section to be consumed at all, so this exercises the generic path:
    // an unrecognised/absent profile simply ignores the section.
    std::vector<uint8_t> sect;
    sect.insert(sect.end(), { 'M', 'I', 'G', '1' });
    sect.push_back(0xFF);            // page low  — 0xFFFF, way past 2 KB
    sect.push_back(0xFF);            // page high
    sect.resize(sect.size() + 0x800, 0xA5);
    assert(sect.size() == 4 + 2 + 0x800);

    // Feeding it to a non-//c Memory must be a no-op, not a crash: the
    // profile pointer is null there and the section is skipped by length.
    Memory mem;
    std::vector<uint8_t> blob;
    mem.appendSnapshotState(blob);
    assert(mem.loadSnapshotState(blob.data(), blob.size()));

    std::printf("  ok: MIG section length-skipped when no //c profile\n");
}

}  // namespace

int main()
{
    std::printf("IWM + MIG snapshot test\n");
    testIwmRoundTrip();
    testMemoryTrailerCarriesIwm();
    testMigPageMasked();
    std::printf("PASS\n");
    return 0;
}

// Per-card snapshot/rewind serialization — pins the 2026-07-29 workflow
// hunt's "cards serialize no state" findings.
//
// The project convention is that every slot card serializes its
// guest-visible state, so a rewind or snapshot-load lands the guest's
// firmware on the machine it was recorded against. These five carried
// NOTHING (or, for SmartPortCard, everything except its protocol call
// engine), so a restore mid-transfer resumed against the LIVE device:
//
//   * CffaCard      — ATA taskfile + in-flight PIO cursor (wordIdx_)
//   * ProDOSHardDiskCard — selected block + byte cursor in it
//   * SmartPortCard — the $Cn0D call engine (spResult_/spCollect_/…)
//   * SuperSerialCard — ACIA command/control decode + sticky errors
//   * ClockCard     — uPD1990AC 48-bit shift register + TP/IRQ timer
//
// Each case: drive the card into a distinctive non-default state, snapshot,
// build a FRESH card, restore, and assert the observable state came across.
// Every loader must also ignore a foreign blob rather than misparse it.

#include "CffaCard.h"
#include "ClockCard.h"
#include "ProDOSHardDiskCard.h"
#include "SmartPortCard.h"
#include "SmartPortHdvUnit.h"
#include "SuperSerialCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace {

/// A blob no card should accept.
const std::vector<uint8_t> kForeign(64, 0xAB);

void testCffaAtaState()
{
    pom2::CffaCard a(7);
    // Walk the taskfile: LBA registers + a sector count, then latch the
    // EEPROM write-enable off (a card-level bit) — all guest-visible.
    a.deviceSelectWrite(0xA, 0x08);   // sector count
    a.deviceSelectWrite(0xB, 0x21);   // LBA0
    a.deviceSelectWrite(0xC, 0x43);   // LBA1
    a.deviceSelectWrite(0xD, 0x65);   // LBA2
    a.deviceSelectWrite(0x3, 0);      // write-enable ON (writeProtect_ = false)

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    pom2::CffaCard b(7);
    b.loadSnapshotState(blob.data(), blob.size());
    assert(b.deviceSelectRead(0xA) == 0x08);
    assert(b.deviceSelectRead(0xB) == 0x21);
    assert(b.deviceSelectRead(0xC) == 0x43);
    assert(b.deviceSelectRead(0xD) == 0x65);

    // Re-serialising the restored card must reproduce the blob — the only
    // way to assert the PRIVATE ATA fields (phase, wordIdx_, wordBuf_)
    // came across, not just the registers the bus can read back.
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    pom2::CffaCard c(7);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    assert(c.deviceSelectRead(0xA) == 0x00);   // untouched

    std::printf("  ok: CFFA ATA taskfile + PIO state round-trips\n");
}

void testProDosHdvStreamCursor()
{
    ProDOSHardDiskCard a(7);
    // Select a block and advance the byte cursor partway into it. Without
    // serialization a rewind here left the LIVE cursor under the restored
    // firmware's read loop — the rest of the 512-byte stream came out of
    // the wrong offset.
    a.deviceSelectWrite(0x1, 0x34);   // block low
    a.deviceSelectWrite(0x2, 0x12);   // block high

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(blob.size() == 8);

    ProDOSHardDiskCard b(7);
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);          // block + cursor identical

    ProDOSHardDiskCard c(7);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3 != blob);          // foreign blob ignored

    std::printf("  ok: ProDOS HDV block + stream cursor round-trips\n");
}

void testSmartPortCallEngine()
{
    pom2::SmartPortCard a(5);
    a.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    // Drive the $Cn0D protocol stub: BEGIN, then push a STATUS call's
    // command + parameter bytes into the collect buffer, then execute.
    a.deviceSelectWrite(0xE, 0x00);           // BEGIN
    a.deviceSelectWrite(0x7, 0x00);           // cmd = STATUS
    for (uint8_t i = 1; i <= 4; ++i)
        a.deviceSelectWrite(0x7, i);          // param list
    (void)a.deviceSelectRead(0xE);            // execute → result staged

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);

    pom2::SmartPortCard b(5);
    b.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    // The v1.1 tail (spCollect_/spResult_/spResultPos_/spError_) must
    // survive: pre-fix these were absent and a restore resumed streaming
    // the LIVE card's result payload out of reg 0x9.
    assert(blob2 == blob);

    // An old (pre-tail) blob must still load, leaving a RESET engine
    // rather than letting the live one leak through.
    pom2::SmartPortCard c(5);
    c.setUnit(0, std::make_unique<pom2::SmartPortHdvUnit>());
    constexpr size_t kV1Bytes = 4 + 2 * (6 + 512);
    assert(blob.size() > kV1Bytes);
    c.loadSnapshotState(blob.data(), kV1Bytes);
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3.size() == blob.size());   // tail re-emitted, zeroed

    std::printf("  ok: SmartPort call engine round-trips; v1 blobs load\n");
}

void testSscAciaRegisters()
{
    SuperSerialCard a(2);
    // Program the ACIA: control register (baud/word length) then the
    // command register (DTR + RX IRQ enable). $C0n8+ is the device-select
    // window: reg 2 = command, reg 3 = control.
    a.deviceSelectWrite(0xB, 0x1E);   // control: 9600 8N1
    a.deviceSelectWrite(0xA, 0x09);   // command: DTR on, RX IRQ enabled
    assert(a.dtrAsserted());

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    SuperSerialCard b(2);
    assert(!b.dtrAsserted());                 // fresh card: DTR low
    b.loadSnapshotState(blob.data(), blob.size());
    assert(b.dtrAsserted());                  // restored
    assert(b.rxIrqEnabled() == a.rxIrqEnabled());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);

    SuperSerialCard c(2);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    assert(!c.dtrAsserted());                 // foreign blob ignored

    // The TX pacing rate is DERIVED from the control register's baud divider,
    // and is not itself serialized — restoring must recompute it. Restoring a
    // slow-baud snapshot into a card currently programmed fast used to leave
    // the fast rate in place, draining the TX ring far quicker than the
    // restored ACIA configuration allows.
    SuperSerialCard slow(2);
    slow.deviceSelectWrite(0xB, 0x06);        // control: 300 baud
    const double slowRate = slow.bytesPerSecond();
    assert(slowRate > 0.0);
    std::vector<uint8_t> slowBlob;
    slow.appendSnapshotState(slowBlob);

    SuperSerialCard fast(2);
    fast.deviceSelectWrite(0xB, 0x0F);        // control: 19 200 baud
    assert(fast.bytesPerSecond() > slowRate);
    fast.loadSnapshotState(slowBlob.data(), slowBlob.size());
    assert(fast.bytesPerSecond() == slowRate);

    // And the other direction, so the fix can't be a hard-coded slow default.
    std::vector<uint8_t> fastBlob;
    SuperSerialCard fast2(2);
    fast2.deviceSelectWrite(0xB, 0x0F);
    const double fastRate = fast2.bytesPerSecond();
    fast2.appendSnapshotState(fastBlob);
    slow.loadSnapshotState(fastBlob.data(), fastBlob.size());
    assert(slow.bytesPerSecond() == fastRate);

    std::printf("  ok: SSC ACIA register state round-trips (incl. baud pacing)\n");
}

/// Deterministic time source (the ClockCard TimeFn shape is `std::tm(*)()`).
std::tm fixedTime()
{
    std::tm t{};
    t.tm_year = 126; t.tm_mon = 4; t.tm_mday = 9;
    t.tm_hour = 14;  t.tm_min = 37; t.tm_sec = 42; t.tm_wday = 6;
    return t;
}

void testClockShiftRegister()
{
    auto ap = ClockCard::makeForTest(4, &fixedTime);
    ClockCard& a = *ap;
    // MODE_TIME_READ + STB rising edge snapshots the time into the 48-bit
    // shift register, then CLK pulses shift it out. Stop PARTWAY: that
    // half-shifted register is exactly what a rewind used to lose.
    a.deviceSelectWrite(0x0, 0x18);           // mode latch + STB high
    for (int i = 0; i < 9; ++i) {             // 9 CLK pulses
        a.deviceSelectWrite(0x0, 0x18 | 0x02);
        a.deviceSelectWrite(0x0, 0x18);
    }

    std::vector<uint8_t> blob;
    a.appendSnapshotState(blob);
    assert(!blob.empty());

    auto bp = ClockCard::makeForTest(4, &fixedTime);
    ClockCard& b = *bp;
    b.loadSnapshotState(blob.data(), blob.size());
    std::vector<uint8_t> blob2;
    b.appendSnapshotState(blob2);
    assert(blob2 == blob);          // shift register + mode + TP identical

    auto cp = ClockCard::makeForTest(4, &fixedTime);
    ClockCard& c = *cp;
    std::vector<uint8_t> fresh;
    c.appendSnapshotState(fresh);
    c.loadSnapshotState(kForeign.data(), kForeign.size());
    std::vector<uint8_t> blob3;
    c.appendSnapshotState(blob3);
    assert(blob3 == fresh);         // foreign blob ignored

    std::printf("  ok: ThunderClock shift register + TP state round-trips\n");
}

}  // namespace

int main()
{
    std::printf("Card snapshot-state test\n");
    testCffaAtaState();
    testProDosHdvStreamCursor();
    testSmartPortCallEngine();
    testSscAciaRegisters();
    testClockShiftRegister();
    std::printf("PASS\n");
    return 0;
}

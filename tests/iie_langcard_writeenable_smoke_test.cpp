// Apple //e Language-Card write-enable conformance test.
//
// Pins POM2's $C08x decode to MAME's `apple2e.cpp:1506-1564` lc_update():
//   - any EVEN access clears prewrite AND write-enable;
//   - any WRITE clears prewrite only — write-enable is left UNCHANGED;
//   - first odd READ arms prewrite, a second consecutive odd READ commits
//     write-enable.
//
// The previous POM2 formula (`writeEnable = odd && prevPrewrite`, recomputed
// every access) diverged: it dropped/re-armed write-enable on repeated odd
// writes/reads. That silently dropped Language-Card RAM writes for software
// that streams data into LC RAM while toggling banks — the Nox Archaist
// city/save decompressor (into aux LC at $D000) corrupted its own $D000 code
// and crashed (PC ran into a bogus zero-page trampoline). Found by diffing
// POM2 against MAME (project "MAME source of truth" rule).
//
// Test is behavioural: enable LC RAM, write a byte, read it back. write-enable
// state is observed through whether the write to $D000 persisted.

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {
int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

// $C083: bank2, mode 3 (read-RAM + odd/write-candidate). Two odd reads = the
// classic "RAM read + write enable" sequence.
void rd(Memory& m, uint16_t a) { (void)m.memRead(a); }
void wr(Memory& m, uint16_t a) { m.memWrite(a, 0); }
}  // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);

    // ── A) Classic enable: two odd READs ⇒ LC RAM readable + writable ─────
    rd(mem, 0xC083);                 // arm prewrite
    rd(mem, 0xC083);                 // commit write-enable
    mem.memWrite(0xD000, 0xAA);
    CHECK(mem.memRead(0xD000) == 0xAA, "classic enable (LDA $C083 x2): LC write takes");

    // ── B) THE FIX: write-enable is STICKY across WRITES to an odd switch ──
    // STA $C083 clears prewrite but must NOT clear write-enable (MAME). The
    // old formula cleared it on the 2nd STA, dropping this write.
    wr(mem, 0xC083);                 // STA $C083
    wr(mem, 0xC083);                 // STA $C083 again
    mem.memWrite(0xD000, 0xBB);
    CHECK(mem.memRead(0xD000) == 0xBB,
          "write-enable held across repeated STA $C08x (MAME lc_update)");

    // ── C) EVEN access disables write-enable; one odd read does NOT re-arm ─
    rd(mem, 0xC083); rd(mem, 0xC083);   // re-enable (write-enable on)
    rd(mem, 0xC082);                    // even access ⇒ prewrite + WE cleared
    rd(mem, 0xC083);                    // single odd read ⇒ arms prewrite only
    mem.memWrite(0xD000, 0xCC);        // write-enable still OFF ⇒ dropped
    CHECK(mem.memRead(0xD000) == 0xBB,
          "even access disables write-enable; single odd read does not re-enable");

    // ── D) read-then-write must NOT enable (a write only clears prewrite) ──
    rd(mem, 0xC082);                    // even ⇒ clear WE/prewrite (ROM read)
    rd(mem, 0xC083);                    // odd read ⇒ arm prewrite, RAM read on
    wr(mem, 0xC083);                    // STA ⇒ clears prewrite, WE stays OFF
    mem.memWrite(0xD000, 0xDD);        // dropped
    CHECK(mem.memRead(0xD000) == 0xBB,
          "LDA then STA $C08x does NOT enable LC writes");

    if (failures == 0) {
        std::printf("iie_langcard_writeenable_smoke OK\n");
        return 0;
    }
    std::printf("iie_langcard_writeenable_smoke: %d FAILURE(S)\n", failures);
    return 1;
}

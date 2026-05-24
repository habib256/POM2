// Floating-bus PAGE2 / 80STORE gating regression test.
//
// The video scanner (and therefore Memory::floatingBus, which mimics it
// for undriven soft-switch reads) honours PAGE2 only when 80STORE is OFF.
// With 80STORE ON, PAGE2 redirects aux-bank selection instead of the
// displayed page, so the scanner always fetches page 1. MAME models this
// as `use_page_2() = m_page2 && !m_80store` (apple2video.cpp).
//
// Regression: floatingBus computed `Page2 = page2` unconditionally, so any
// 80-column / DHGR program (80STORE on) saw the floating bus read the
// wrong DRAM page. This test fills the two HGR pages in MAIN RAM with
// distinct sentinels and checks which page floatingBus samples under the
// 80STORE on/off combinations.
//
// Core test: no ImGui, no CPU — drives Memory directly.

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { \
    std::printf("FAIL: %s\n", msg); ++failures; } } while (0)

}  // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);

    // Fill the whole HGR page-1 ($2000-$3FFF) and page-2 ($4000-$5FFF)
    // ranges in MAIN RAM with distinct sentinels, so floatingBus returns a
    // page-identifying byte regardless of the exact scanner address.
    uint8_t* main = const_cast<uint8_t*>(mem.data());
    for (int a = 0x2000; a <= 0x3FFF; ++a) main[a] = 0xA1;   // page 1
    for (int a = 0x4000; a <= 0x5FFF; ++a) main[a] = 0xB2;   // page 2

    // Display: graphics (text off), HIRES on, MIXED off, PAGE2 on.
    mem.memWrite(0xC050, 0);    // TEXT off → graphics
    mem.memWrite(0xC052, 0);    // MIXED off
    mem.memWrite(0xC057, 0);    // HIRES on
    mem.memWrite(0xC055, 0);    // PAGE2 on

    // 80STORE OFF + PAGE2 on → scanner uses page 2 → 0xB2 (unchanged by fix).
    mem.memWrite(0xC000, 0);    // 80STORE off
    CHECK(mem.peekFloatingBus() == 0xB2, "80STORE off + PAGE2 -> page 2 (0xB2)");

    // 80STORE ON + PAGE2 on → scanner forced to page 1 → 0xA1 (the fix).
    mem.memWrite(0xC001, 0);    // 80STORE on
    CHECK(mem.peekFloatingBus() == 0xA1, "80STORE on + PAGE2 -> page 1 (0xA1)");

    // Sanity: with 80STORE on, clearing PAGE2 still reads page 1.
    mem.memWrite(0xC054, 0);    // PAGE2 off
    CHECK(mem.peekFloatingBus() == 0xA1, "80STORE on, PAGE2 off -> page 1 (0xA1)");

    if (failures == 0) std::printf("OK floatingbus_page2_smoke\n");
    return failures == 0 ? 0 : 1;
}

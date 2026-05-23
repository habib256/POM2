// //e auxiliary-memory paging conformance test.
//
// Motivated by Nox Archaist freezing entering a city: a 128K LZSS decompressor
// (input in main RAM, output to aux via RAMWRT, back-references read from aux
// output via RAMRD toggles) loops forever on POM2 though the loaded data is
// byte-correct and the 65C02 passes Klaus. This pins POM2's //e MMU routing
// against the documented Apple //e behaviour (MAME `apple2e.cpp` a2_mmu /
// `apple2e.cpp:1700-1780` read/write bank selection; IIe Tech Ref Ch.4):
//
//   $0000-$01FF      ALTZP            → aux else main           (read & write)
//   $0200-$03FF      RAMRD/RAMWRT     → aux else main
//   $0400-$07FF      80STORE on       → PAGE2 picks aux/main; else RAMRD/RAMWRT
//   $0800-$1FFF      RAMRD/RAMWRT
//   $2000-$3FFF      80STORE+HIRES on → PAGE2 picks aux/main; else RAMRD/RAMWRT
//   $4000-$BFFF      RAMRD/RAMWRT
//
// Also: the $C000-$C00F paging switches are WRITE-only (a read must NOT toggle
// them), and the exact (zp),Y pattern the decompressor uses must read the
// right bank.
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

// Soft-switch helpers — these are the real bus accesses software uses.
void wr(Memory& m, uint16_t a, uint8_t v) { m.memWrite(a, v); }
uint8_t rd(Memory& m, uint16_t a) { return m.memRead(a); }

// Paging switches (write-only on //e).
void ramRd (Memory& m, bool on) { m.memWrite(on ? 0xC003 : 0xC002, 0); }
void ramWrt(Memory& m, bool on) { m.memWrite(on ? 0xC005 : 0xC004, 0); }
void store80(Memory& m, bool on) { m.memWrite(on ? 0xC001 : 0xC000, 0); }
void altZp (Memory& m, bool on) { m.memWrite(on ? 0xC009 : 0xC008, 0); }
// Display switches (read or write toggles; they also drive memory under 80STORE).
void page2 (Memory& m, bool on) { m.memWrite(on ? 0xC055 : 0xC054, 0); }
void hires (Memory& m, bool on) { m.memWrite(on ? 0xC057 : 0xC056, 0); }

// Write a sentinel through the bus, then check which physical bank received it
// by inspecting the raw arrays (data()=main, auxData()=aux).
void expectWriteBank(Memory& m, uint16_t addr, bool toAux, const char* tag)
{
    const uint8_t* main = m.data();
    const uint8_t* aux  = m.auxData();
    // Clear both banks at addr so we can see where the write landed.
    const_cast<uint8_t*>(main)[addr] = 0x00;
    const_cast<uint8_t*>(aux )[addr] = 0x00;
    wr(m, addr, 0x5A);
    if (toAux) {
        CHECK(aux[addr] == 0x5A && main[addr] == 0x00, tag);
    } else {
        CHECK(main[addr] == 0x5A && aux[addr] == 0x00, tag);
    }
}

// Seed both banks at addr distinctly, then check which the bus read returns.
void expectReadBank(Memory& m, uint16_t addr, bool fromAux, const char* tag)
{
    const_cast<uint8_t*>(m.data())[addr]    = 0x11;   // main
    const_cast<uint8_t*>(m.auxData())[addr] = 0x22;   // aux
    const uint8_t got = rd(m, addr);
    CHECK(got == (fromAux ? 0x22 : 0x11), tag);
}

}  // namespace

int main()
{
    Memory mem;
    mem.setIIEMode(true);

    // ── $0200-$03FF, $0800-$1FFF, $4000-$BFFF: plain RAMRD/RAMWRT ─────────
    for (uint16_t a : {uint16_t(0x0300), uint16_t(0x1234), uint16_t(0x4000),
                       uint16_t(0xB7EE), uint16_t(0xBFFF)}) {
        ramWrt(mem, false); expectWriteBank(mem, a, false, "RAMWRT off -> main write");
        ramWrt(mem, true);  expectWriteBank(mem, a, true,  "RAMWRT on  -> aux write");
        ramRd (mem, false); expectReadBank (mem, a, false, "RAMRD off  -> main read");
        ramRd (mem, true);  expectReadBank (mem, a, true,  "RAMRD on   -> aux read");
    }
    ramRd(mem, false); ramWrt(mem, false);

    // ── $0000-$01FF: ALTZP (zero page + stack) ───────────────────────────
    for (uint16_t a : {uint16_t(0x00EC), uint16_t(0x01F5)}) {
        altZp(mem, false); expectWriteBank(mem, a, false, "ALTZP off -> main ZP/stack write");
        altZp(mem, false); expectReadBank (mem, a, false, "ALTZP off -> main ZP/stack read");
        altZp(mem, true);  expectWriteBank(mem, a, true,  "ALTZP on  -> aux ZP/stack write");
        altZp(mem, true);  expectReadBank (mem, a, true,  "ALTZP on  -> aux ZP/stack read");
    }
    altZp(mem, false);

    // ── $0400-$07FF: 80STORE makes PAGE2 pick the bank (overrides RAMRD/WRT) ─
    {
        const uint16_t a = 0x0500;
        store80(mem, true);
        // Even with RAMRD/RAMWRT pointing at aux, PAGE2=off must select main.
        ramRd(mem, true); ramWrt(mem, true);
        page2(mem, false);
        expectWriteBank(mem, a, false, "80STORE+PAGE2off -> main write ($400-7FF)");
        expectReadBank (mem, a, false, "80STORE+PAGE2off -> main read  ($400-7FF)");
        page2(mem, true);
        expectWriteBank(mem, a, true,  "80STORE+PAGE2on  -> aux write  ($400-7FF)");
        expectReadBank (mem, a, true,  "80STORE+PAGE2on  -> aux read   ($400-7FF)");
        store80(mem, false); page2(mem, false); ramRd(mem, false); ramWrt(mem, false);
    }

    // ── $2000-$3FFF: PAGE2 only picks the bank when 80STORE AND HIRES on ──
    {
        const uint16_t a = 0x2001;            // the exact Nox input-buffer addr
        store80(mem, true); page2(mem, true);
        // HIRES off: $2000-$3FFF must IGNORE PAGE2 and use RAMRD/RAMWRT.
        hires(mem, false);
        ramRd(mem, false); ramWrt(mem, false);
        expectWriteBank(mem, a, false, "80STORE+PAGE2on+HIRESoff -> RAMWRT main write ($2000)");
        expectReadBank (mem, a, false, "80STORE+PAGE2on+HIRESoff -> RAMRD  main read  ($2000)");
        // HIRES on: now PAGE2 selects aux.
        hires(mem, true);
        expectWriteBank(mem, a, true,  "80STORE+HIRES+PAGE2on -> aux write ($2000)");
        expectReadBank (mem, a, true,  "80STORE+HIRES+PAGE2on -> aux read  ($2000)");
        store80(mem, false); hires(mem, false); page2(mem, false);
        ramRd(mem, false); ramWrt(mem, false);
    }

    // ── $C000-$C00F paging switches are WRITE-ONLY: a READ must not toggle ─
    {
        ramRd(mem, false);                 // RAMRD off
        (void)rd(mem, 0xC003);             // READ "RAMRD on" — must NOT enable it
        expectReadBank(mem, 0x4000, false, "read of $C003 must NOT toggle RAMRD on");
        ramWrt(mem, false);
        (void)rd(mem, 0xC005);             // READ "RAMWRT on" — must NOT enable it
        expectWriteBank(mem, 0x4000, false, "read of $C005 must NOT toggle RAMWRT on");
    }

    // ── The exact Nox decompressor pattern ───────────────────────────────
    // Output to aux $B7EE (RAMWRT on), read the back-reference from aux
    // (RAMRD on), read the literal input from main $2001 (RAMRD off). All
    // three must hit the intended bank or the decompressor desyncs.
    {
        const_cast<uint8_t*>(mem.data())[0x2001]    = 0xAB;  // main input byte
        const_cast<uint8_t*>(mem.auxData())[0x2001] = 0xCD;  // aux shadow (wrong)
        ramWrt(mem, true);
        wr(mem, 0xB7EE, 0x42);                 // output -> aux
        CHECK(mem.auxData()[0xB7EE] == 0x42, "decompressor output lands in aux");
        ramRd(mem, true);
        CHECK(rd(mem, 0xB7EE) == 0x42, "back-reference reads aux output");
        ramRd(mem, false);
        CHECK(rd(mem, 0x2001) == 0xAB, "literal input reads main, not aux shadow");
        ramWrt(mem, false);
    }

    if (failures == 0) {
        std::printf("iie_aux_paging_conformance OK\n");
        return 0;
    }
    std::printf("iie_aux_paging_conformance: %d FAILURE(S)\n", failures);
    return 1;
}

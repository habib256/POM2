// PrinterCard smoke test — pins the synthetic parallel printer card's
// three observable surfaces:
//
//   1. Slot ROM fingerprint — the PR#n trampoline at $Cn00, the Pascal
//      autodetect bytes, and the output handler at $Cn31 must match the
//      header's documented layout. Anything that shifts these bytes
//      breaks every Apple II program that uses `PR#N` against this card.
//   2. Data port spool — a write to $C0(8+s)1 enqueues the raw byte; a
//      read from any device-select offset returns $FF (always ready);
//      spoolText() does the Apple II 7-bit text rendering (strip bit 7,
//      CR → LF, NUL dropped) the UI relies on.
//   3. CPU-driven `PR#1 : PRINT` flow — a small 6502 program JSRs the
//      slot ROM hook ($C100) to install CSWL/CSWH, then "prints" two
//      characters + a CR by JSR'ing the output handler directly. The
//      CSWL/CSWH zero-page bytes and the spool contents are then
//      asserted. This is the real proof that the synthetic ROM
//      executes correctly under the CPU — not just that the bytes are
//      there.

#include "M6502.h"
#include "Memory.h"
#include "PrinterCard.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>

namespace {

void testRomFingerprint()
{
    PrinterCard card(1);

    // PR#n entry at $Cn00 — JMP $Cn20 (skip the Pascal sig region).
    assert(card.slotRomRead(0x00) == 0x4C);
    assert(card.slotRomRead(0x01) == 0x20);
    assert(card.slotRomRead(0x02) == 0xC1);     // slotHi for slot 1

    // Pascal 1.1 autodetect signature.
    assert(card.slotRomRead(0x05) == 0x38);     // SEC
    assert(card.slotRomRead(0x07) == 0x18);     // CLC
    assert(card.slotRomRead(0x0B) == 0x01);     // firmware revision
    assert(card.slotRomRead(0x0C) == 0x00);     // device class = printer

    // CSWL/CSWH installation at $Cn20: LDA #$31 / STA $36 / LDA #$C1 / STA $37 / RTS.
    assert(card.slotRomRead(0x20) == 0xA9);     // LDA #
    assert(card.slotRomRead(0x21) == 0x31);     // low byte of $Cn31
    assert(card.slotRomRead(0x22) == 0x85);     // STA zp
    assert(card.slotRomRead(0x23) == 0x36);     // CSWL
    assert(card.slotRomRead(0x24) == 0xA9);     // LDA #
    assert(card.slotRomRead(0x25) == 0xC1);     // slotHi
    assert(card.slotRomRead(0x26) == 0x85);     // STA zp
    assert(card.slotRomRead(0x27) == 0x37);     // CSWH
    assert(card.slotRomRead(0x28) == 0x60);     // RTS

    // Output handler at $Cn31: STA $C091 / RTS — write A to data port (low4=1).
    assert(card.slotRomRead(0x31) == 0x8D);     // STA abs
    assert(card.slotRomRead(0x32) == 0x91);     // low byte = $80 + slot*16 + 1
    assert(card.slotRomRead(0x33) == 0xC0);     // high byte
    assert(card.slotRomRead(0x34) == 0x60);     // RTS

    // A card built at slot 3 must rebake the slot-dependent bytes.
    PrinterCard card3(3);
    assert(card3.slotRomRead(0x02) == 0xC3);            // slotHi
    assert(card3.slotRomRead(0x25) == 0xC3);            // slotHi
    assert(card3.slotRomRead(0x32) == 0xB1);            // $80 + 3*16 + 1 = $B1

    std::printf("  ok: slot ROM fingerprint\n");
}

void testDataPortSpool()
{
    PrinterCard card(1);

    // Reads on any device-select offset return $FF (always ready).
    for (uint8_t i = 0; i < 16; ++i)
        assert(card.deviceSelectRead(i) == 0xFF);

    // Writes to offset 1 (data port) enqueue the byte verbatim — no
    // high-bit strip at the card level.
    card.deviceSelectWrite(1, 0xC8);    // 'H' | 0x80
    card.deviceSelectWrite(1, 0xC9);    // 'I'
    card.deviceSelectWrite(1, 0x8D);    // CR
    assert(card.bytesWritten() == 3);
    const auto raw = card.spoolBytes();
    assert(raw.size() == 3);
    assert(raw[0] == 0xC8 && raw[1] == 0xC9 && raw[2] == 0x8D);

    // Writes to other offsets are ignored (no-op).
    card.deviceSelectWrite(0, 0xFF);
    card.deviceSelectWrite(2, 0xFF);
    card.deviceSelectWrite(15, 0xFF);
    assert(card.bytesWritten() == 3);

    // spoolText: bit 7 stripped, CR → LF, NULs dropped.
    card.clearSpool();
    card.deviceSelectWrite(1, 0xC8);    // 'H'
    card.deviceSelectWrite(1, 0x00);    // NUL — should drop
    card.deviceSelectWrite(1, 0xC9);    // 'I'
    card.deviceSelectWrite(1, 0x8D);    // CR → LF
    card.deviceSelectWrite(1, 0xCA);    // 'J'
    assert(card.spoolText() == "HI\nJ");

    card.clearSpool();
    assert(card.bytesWritten() == 0);
    assert(card.spoolText().empty());

    std::printf("  ok: data port + spool semantics\n");
}

// CPU integration: drive the ROM hook + output handler the way `PR#1` +
// a few COUT calls would on real hardware. Mimics the user-visible
// "PR#1 : PRINT \"HI\"" flow without needing the Apple ROM (which we'd
// otherwise need to load just to call $FDED).
void testCpuPrintFlow()
{
    Memory mem;
    M6502  cpu(&mem);

    // Plug the printer card into slot 1 — the dispatch path Memory uses
    // for $C100-$C1FF reads and $C090-$C09F device-select.
    auto card = std::make_unique<PrinterCard>(1);
    PrinterCard* cardPtr = card.get();
    mem.slotBus().plug(1, std::move(card));

    // Sanity precondition: CSWL/CSWH start at $00 so we can prove the
    // ROM hook actually wrote them.
    mem.writeRamUnchecked(0x36, 0x00);
    mem.writeRamUnchecked(0x37, 0x00);

    // Program at $0800 — JSR $C100 then 3× JSR $C131 with 'H', 'I', CR.
    //
    //   $0800  20 00 C1   JSR $C100        ; PR#1 hook → CSWL=$31, CSWH=$C1
    //   $0803  A9 C8      LDA #$C8         ; 'H' | $80
    //   $0805  20 31 C1   JSR $C131        ; output handler → STA $C091
    //   $0808  A9 C9      LDA #$C9         ; 'I' | $80
    //   $080A  20 31 C1   JSR $C131
    //   $080D  A9 8D      LDA #$8D         ; CR | $80
    //   $080F  20 31 C1   JSR $C131
    //   $0812  4C 12 08   JMP $0812        ; park
    const uint8_t program[] = {
        0x20, 0x00, 0xC1,
        0xA9, 0xC8,
        0x20, 0x31, 0xC1,
        0xA9, 0xC9,
        0x20, 0x31, 0xC1,
        0xA9, 0x8D,
        0x20, 0x31, 0xC1,
        0x4C, 0x12, 0x08,
    };
    for (size_t i = 0; i < sizeof(program); ++i)
        mem.writeRamUnchecked(static_cast<uint16_t>(0x0800 + i), program[i]);

    cpu.setProgramCounter(0x0800);

    // Run a few hundred steps — the program reaches the JMP loop well
    // before this budget is exhausted (~20 instructions plus the 3
    // returns from the output handler).
    for (int i = 0; i < 500; ++i) cpu.step();

    // The PR#1 trampoline must have installed our CSWL/CSWH.
    const uint8_t cswl = mem.peekMainRam(0x36);
    const uint8_t cswh = mem.peekMainRam(0x37);
    assert(cswl == 0x31);
    assert(cswh == 0xC1);

    // The three JSR $C131 calls must have spooled three bytes.
    assert(cardPtr->bytesWritten() == 3);
    const auto raw = cardPtr->spoolBytes();
    assert(raw[0] == 0xC8 && raw[1] == 0xC9 && raw[2] == 0x8D);
    assert(cardPtr->spoolText() == "HI\n");

    // CPU should be parked in the JMP $0812 loop.
    assert(cpu.getProgramCounter() == 0x0812);

    std::printf("  ok: CPU-driven PR#1 hook + 3 COUT-style writes\n");
}

} // namespace

int main()
{
    std::printf("PrinterCard smoke test\n");
    testRomFingerprint();
    testDataPortSpool();
    testCpuPrintFlow();
    std::printf("PASS\n");
    return 0;
}

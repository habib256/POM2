// MouseCardAppleWin smoke test — pins the AppleWin HLE port (the
// non-MAME variant that synthesises the MCU side instead of running the
// 341-0269 mask ROM). Three things checked:
//
//   1. Construction with a synthetic 2 KB slot ROM works; loadRom rejects
//      missing files and wrong sizes.
//   2. Slot ROM bank-select via PIA Port B bits 8..10 — matches
//      AppleWin SetSlotRom: `offset = (m_by6821B << 7) & 0x0700`.
//   3. The BIT5 (PB5) write-strobe handshake from On6821_B reaches
//      OnCommand: pulse a MOUSE_HOME byte through the PIA and observe
//      the slot IRQ stays low + the card is alive afterwards (no state
//      corruption). Also pulse MOUSE_INIT and verify the firmware ends
//      up reading 0xFF back on Port A (the canned reply for MOUSE_INIT).

#include "MouseCardAppleWin.h"
#include "Memory.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::string writeTempBlob(const std::vector<uint8_t>& bytes,
                          const std::string& nameSuffix)
{
    const std::string path = "/tmp/pom2_mouseaw_test_" + nameSuffix;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    return path;
}

std::vector<uint8_t> buildBankSignatureRom()
{
    std::vector<uint8_t> rom(0x800, 0);
    for (int bank = 0; bank < 8; ++bank) {
        std::fill(rom.begin() + bank * 0x100,
                  rom.begin() + (bank + 1) * 0x100,
                  static_cast<uint8_t>(0xC0 + bank));
    }
    return rom;
}

void test_missing_rom_refuses_to_load()
{
    MouseCardAppleWin card(4);
    assert(!card.loadRom("/tmp/pom2_does_not_exist_slot_aw.bin"));
    assert(!card.isReady());
}

void test_size_mismatch_refuses_to_load()
{
    MouseCardAppleWin card(4);
    std::vector<uint8_t> half(0x400, 0xAA);
    const auto p = writeTempBlob(half, "half_slot.bin");
    assert(!card.loadRom(p));
    assert(!card.isReady());
    std::remove(p.c_str());
}

// Helper: program the PIA to "all data + DDR=0xFF" on both ports via the
// $C0(8+s)X device-select window. The PIA register select is `addr & 3`.
void primePiaForOutput(Memory& mem, uint16_t devBase)
{
    // CRA = 0 → access DDR at offset 0; DDRA = 0xFF; CRA = 0x04 → data.
    mem.memWrite(devBase + 1, 0x00);
    mem.memWrite(devBase + 0, 0xFF);
    mem.memWrite(devBase + 1, 0x04);
    // Same for B.
    mem.memWrite(devBase + 3, 0x00);
    mem.memWrite(devBase + 2, 0xFF);
    mem.memWrite(devBase + 3, 0x04);
}

void test_slot_rom_bank_select()
{
    const auto slotBytes = buildBankSignatureRom();
    const auto slotPath  = writeTempBlob(slotBytes, "bank_slot.bin");

    Memory mem;
    auto card = std::make_unique<MouseCardAppleWin>(4);
    assert(card->loadRom(slotPath));
    assert(card->isReady());
    MouseCardAppleWin* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    const uint16_t devBase = 0xC0C0;     // slot 4
    primePiaForOutput(mem, devBase);

    // AppleWin On6821_B only reacts when bits in 0x3E change. After reset
    // by6821B = 0x40, so the first PRB write must include enough deltas
    // to clear the prior state. We toggle bank bits 0x0E (PB1..PB3) plus
    // PB5 / PB4 set to "idle" (= high) so the strobe edges don't fire.
    const struct { uint8_t prb; uint8_t expected; } cases[] = {
        { 0x30 | 0x00, 0xC0 },     // bank 0
        { 0x30 | 0x02, 0xC1 },     // bank 1
        { 0x30 | 0x04, 0xC2 },     // bank 2
        { 0x30 | 0x06, 0xC3 },     // bank 3
        { 0x30 | 0x08, 0xC4 },     // bank 4
        { 0x30 | 0x0A, 0xC5 },     // bank 5
        { 0x30 | 0x0C, 0xC6 },     // bank 6
        { 0x30 | 0x0E, 0xC7 },     // bank 7
    };
    for (const auto& c : cases) {
        mem.memWrite(devBase + 2, c.prb);
        const uint8_t got = mem.memRead(0xC400);
        if (got != c.expected) {
            std::fprintf(stderr,
                "bank PRB=$%02X: expected $%02X got $%02X\n",
                c.prb, c.expected, got);
        }
        assert(got == c.expected);
    }

    std::remove(slotPath.c_str());
}

// Drive one command byte through the BIT5 write-strobe handshake:
// "byte on PRA, then PB5 1→0 commits the byte". After the trailing
// edge, On6821_B feeds the byte into OnCommand.
void pulseCommand(Memory& mem, uint16_t devBase, uint8_t cmdByte)
{
    // PRA = command byte.
    mem.memWrite(devBase + 0, cmdByte);
    // PB5 high (with PB4=1 too so the read-strobe doesn't false-fire).
    mem.memWrite(devBase + 2, 0x30);
    // PB5 low — trailing edge consumes the byte.
    mem.memWrite(devBase + 2, 0x10);
}

void test_command_handshake_reaches_oncommand()
{
    // A blank slot ROM is fine — we're poking the HLE side directly.
    std::vector<uint8_t> rom(0x800, 0x00);
    const auto slotPath = writeTempBlob(rom, "blank_slot.bin");

    Memory mem;
    auto card = std::make_unique<MouseCardAppleWin>(4);
    assert(card->loadRom(slotPath));
    MouseCardAppleWin* raw = card.get();
    mem.slotBus().plug(4, std::move(card));
    raw->onReset();

    const uint16_t devBase = 0xC0C0;     // slot 4
    primePiaForOutput(mem, devBase);

    // ── MOUSE_HOME ($70): single-byte command. After it runs the card's
    //    internal position is (0,0); no IRQ should be asserted (mode off).
    pulseCommand(mem, devBase, 0x70);
    assert(!raw->slotIrqAsserted());

    // ── MOUSE_INIT ($50): three-byte command — OnCommand fires after the
    //    first byte and pre-loads byBuff[1] = 0xFF, which gets pushed
    //    onto Port A via pia.setPortAInput(byBuff[1]). Read it back.
    //    Real firmware would flip DDRA → 0 (input) before reading so the
    //    PIA returns the *input* latch, not the output latch. Do the
    //    same here.
    pulseCommand(mem, devBase, 0x50);
    mem.memWrite(devBase + 1, 0x00);     // CRA bit 2 = 0 → DDR access
    mem.memWrite(devBase + 0, 0x00);     // DDRA = all input
    mem.memWrite(devBase + 1, 0x04);     // back to data port
    const uint8_t pra = mem.memRead(devBase + 0);
    if (pra != 0xFF) {
        std::fprintf(stderr,
            "MOUSE_INIT: expected PRA=$FF after OnCommand, got $%02X\n", pra);
    }
    assert(pra == 0xFF);

    std::remove(slotPath.c_str());
}

}  // namespace

int main()
{
    test_missing_rom_refuses_to_load();
    test_size_mismatch_refuses_to_load();
    test_slot_rom_bank_select();
    test_command_handshake_reaches_oncommand();

    std::printf("OK mouse_card_applewin_smoke\n");
    return 0;
}

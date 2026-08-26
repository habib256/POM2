// Built-in input $C0xx contract test.
//
// This deliberately pins the bus-visible behaviour before KeyboardInput and
// PaddleInputs move out of Memory: mirror decode, strobe semantics, modifier
// wiring and exact RC-timer boundaries must survive the extraction unchanged.

#include "Memory.h"

#include <cassert>
#include <cstdint>
#include <cstdio>

namespace {

bool bit7(Memory& memory, uint16_t address)
{
    return (memory.memRead(address) & 0x80) != 0;
}

} // namespace

int main()
{
    // $C000-$C00F are keyboard-latch read mirrors on every model. A read
    // must preserve both the character and strobe; only $C01x acknowledges.
    {
        Memory memory;
        memory.queueKey('Q');
        for (uint16_t address = 0xC000; address <= 0xC00F; ++address)
            assert(memory.memRead(address) == static_cast<uint8_t>(0x80 | 'Q'));
        assert(bit7(memory, 0xC000));
        assert((memory.memRead(0xC01F) & 0x7F) == 'Q');
        assert(!bit7(memory, 0xC000));
    }

    // IIe status reads retain the keyboard character in bits 0..6 and do
    // not acknowledge it. Any write in the $C01x window does.
    {
        Memory memory;
        memory.setIIEMode(true);
        memory.queueKey('z');
        for (uint16_t address = 0xC011; address <= 0xC01F; ++address)
            assert((memory.memRead(address) & 0x7F) == 'z');
        assert(bit7(memory, 0xC000));
        memory.memWrite(0xC01A, 0);
        assert(!bit7(memory, 0xC000));
    }

    // Buttons and their $C069-$C06B mirrors expose state in bit 7 while the
    // low bits remain the floating bus. Mirror pairs read identically when
    // no cycles elapse between them.
    {
        Memory memory;
        memory.setPaddleButton(0, true);
        assert(bit7(memory, 0xC061));
        assert(bit7(memory, 0xC069));
        memory.setPaddleButton(0, false);
        assert(!bit7(memory, 0xC061));

        memory.setOpenAppleKey(true);
        memory.setSolidAppleKey(true);
        assert(bit7(memory, 0xC061));
        assert(bit7(memory, 0xC062));
        memory.setOpenAppleKey(false);
        memory.setSolidAppleKey(false);

        memory.setShiftKey(true);
        assert(!bit7(memory, 0xC063)); // SHK wiring exists only in IIe mode
        memory.setIIEMode(true);
        assert(bit7(memory, 0xC063));
    }

    // The power-on timer is already expired. A $C07x access arms all four
    // RC timers and each value lasts exactly value*11 emulated cycles.
    {
        Memory memory;
        assert(!bit7(memory, 0xC064));
        memory.setPaddle(0, 1);
        memory.setPaddle(1, 0);
        memory.setPaddle(2, 255);

        (void)memory.memRead(0xC07D); // any read in the mirrored window
        assert(bit7(memory, 0xC064));
        assert(!bit7(memory, 0xC065));
        assert(bit7(memory, 0xC066));
        assert(bit7(memory, 0xC06C)); // mirror of paddle 0

        memory.advanceCycles(10);
        assert(bit7(memory, 0xC064));
        memory.advanceCycles(1);
        assert(!bit7(memory, 0xC064));
        memory.advanceCycles(255 * 11 - 11 - 1);
        assert(bit7(memory, 0xC066));
        memory.advanceCycles(1);
        assert(!bit7(memory, 0xC066));

        // Writes strobe the same decoder and restart from the current cycle.
        memory.setPaddle(3, 2);
        memory.memWrite(0xC070, 0xFF);
        assert(bit7(memory, 0xC067));
        memory.advanceCycles(22);
        assert(!bit7(memory, 0xC06F));
    }

    std::printf("input_c0xx_contract_test: OK\n");
    return 0;
}

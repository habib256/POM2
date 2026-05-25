// 65C02 disassembler regression test. The disassembler was NMOS-only while
// the emulator defaults to 65C02, so CMOS opcodes rendered as 1-byte "???",
// desyncing the whole Disasm listing (the classic BBR/BBS-as-1-byte trap).
// This pins the CMOS decode (mnemonic + length) AND the NMOS-mode fallback.

#include "Disassembler6502.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

int main()
{
    std::vector<uint8_t> mem(0x10000, 0);
    auto put = [&](uint16_t a, std::initializer_list<uint8_t> bs) {
        uint16_t p = a; for (uint8_t b : bs) mem[p++] = b;
    };
    auto dis = [&](uint16_t pc, bool cmos, int& len) {
        return pom2::disassemble6502(mem.data(), pc, len, cmos);
    };

    int len = 0;

    // BRA $0312: rel, 2 bytes, target = 0x0302 + 0x10.
    put(0x0300, {0x80, 0x10});
    assert(dis(0x0300, true, len) == "BRA $0312" && len == 2);
    assert(dis(0x0300, false, len) == "???" && len == 1);   // NMOS: no BRA

    // The desync trap: BBR3 $44,$030A — zp + relative, MUST be 3 bytes.
    put(0x0300, {0x3F, 0x44, 0x07});   // 0x3F = BBR3; target = 0x0303 + 0x07 = 0x030A
    assert(dis(0x0300, true, len) == "BBR3 $44,$030A" && len == 3);
    assert(dis(0x0300, false, len) == "???" && len == 1);   // NMOS would desync here

    // BBS7 $10,$02FE — backward relative ($FB = -5): 0x0303 - 5 = 0x02FE.
    put(0x0300, {0xFF, 0x10, 0xFB});
    assert(dis(0x0300, true, len) == "BBS7 $10,$02FE" && len == 3);

    // STZ zp / STZ abs / STZ zp,X / STZ abs,X.
    put(0x0300, {0x64, 0xC0});       assert(dis(0x0300, true, len) == "STZ $C0"     && len == 2);
    put(0x0300, {0x9C, 0x00, 0x20}); assert(dis(0x0300, true, len) == "STZ $2000"   && len == 3);
    put(0x0300, {0x74, 0x30});       assert(dis(0x0300, true, len) == "STZ $30,X"   && len == 2);
    put(0x0300, {0x9E, 0x00, 0x20}); assert(dis(0x0300, true, len) == "STZ $2000,X" && len == 3);

    // INC A / DEC A (accumulator, 1 byte).
    put(0x0300, {0x1A}); assert(dis(0x0300, true, len) == "INC A" && len == 1);
    put(0x0300, {0x3A}); assert(dis(0x0300, true, len) == "DEC A" && len == 1);

    // Stack ops PHX/PHY/PLX/PLY (1 byte).
    put(0x0300, {0xDA}); assert(dis(0x0300, true, len) == "PHX" && len == 1);
    put(0x0300, {0x5A}); assert(dis(0x0300, true, len) == "PHY" && len == 1);
    put(0x0300, {0xFA}); assert(dis(0x0300, true, len) == "PLX" && len == 1);
    put(0x0300, {0x7A}); assert(dis(0x0300, true, len) == "PLY" && len == 1);

    // (zp) indirect and JMP (abs,X).
    put(0x0300, {0xB2, 0x40});       assert(dis(0x0300, true, len) == "LDA ($40)"     && len == 2);
    put(0x0300, {0x7C, 0x00, 0x30}); assert(dis(0x0300, true, len) == "JMP ($3000,X)" && len == 3);

    // RMB/SMB (zp, 2 bytes) and TSB/TRB.
    put(0x0300, {0x07, 0x10}); assert(dis(0x0300, true, len) == "RMB0 $10" && len == 2);
    put(0x0300, {0xF7, 0x10}); assert(dis(0x0300, true, len) == "SMB7 $10" && len == 2);
    put(0x0300, {0x04, 0x20}); assert(dis(0x0300, true, len) == "TSB $20"  && len == 2);
    put(0x0300, {0x14, 0x20}); assert(dis(0x0300, true, len) == "TRB $20"  && len == 2);

    // NMOS opcodes must still decode the same in both modes.
    put(0x0300, {0xA9, 0x42});       assert(dis(0x0300, true,  len) == "LDA #$42" && len == 2);
    put(0x0300, {0x4C, 0x00, 0xFF}); assert(dis(0x0300, false, len) == "JMP $FF00" && len == 3);

    std::printf("OK disasm_cmos (65C02 decode + lengths, NMOS fallback)\n");
    return 0;
}

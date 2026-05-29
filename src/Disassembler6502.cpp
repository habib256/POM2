// POM2 Apple II Emulator
// Copyright (C) 2000-2026 Verhille Arnaud

#include "Disassembler6502.h"

#include <cstdio>

namespace pom2 {
namespace {

// 6502 addressing modes for disassembly formatting
enum AddrMode {
    AM_IMP, AM_IMM, AM_ZP, AM_ZPX, AM_ZPY,
    AM_ABS, AM_ABX, AM_ABY, AM_IND,
    AM_IZX, AM_IZY, AM_REL,
    // 65C02 additions:
    AM_ACC,   // accumulator, rendered "INC A"
    AM_IZP,   // (zp)        — 2 bytes
    AM_IAX,   // (abs,X)     — 3 bytes  (JMP)
    AM_ZPR    // zp,rel      — 3 bytes  (BBR/BBS): "$zz,$tttt"
};

struct OpcodeInfo {
    const char* mnemonic;
    AddrMode mode;
};

// Complete 6502 opcode table (256 entries). Unofficial / illegal opcodes
// are rendered as "???" so a decoded byte is always traceable.
constexpr OpcodeInfo opcodeInfo[256] = {
    {"BRK",AM_IMP}, {"ORA",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // 00-03
    {"???",AM_IMP}, {"ORA",AM_ZP},  {"ASL",AM_ZP},  {"???",AM_IMP},  // 04-07
    {"PHP",AM_IMP}, {"ORA",AM_IMM}, {"ASL",AM_IMP}, {"???",AM_IMP},  // 08-0B
    {"???",AM_IMP}, {"ORA",AM_ABS}, {"ASL",AM_ABS}, {"???",AM_IMP},  // 0C-0F
    {"BPL",AM_REL}, {"ORA",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // 10-13
    {"???",AM_IMP}, {"ORA",AM_ZPX}, {"ASL",AM_ZPX}, {"???",AM_IMP},  // 14-17
    {"CLC",AM_IMP}, {"ORA",AM_ABY}, {"???",AM_IMP}, {"???",AM_IMP},  // 18-1B
    {"???",AM_IMP}, {"ORA",AM_ABX}, {"ASL",AM_ABX}, {"???",AM_IMP},  // 1C-1F
    {"JSR",AM_ABS}, {"AND",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // 20-23
    {"BIT",AM_ZP},  {"AND",AM_ZP},  {"ROL",AM_ZP},  {"???",AM_IMP},  // 24-27
    {"PLP",AM_IMP}, {"AND",AM_IMM}, {"ROL",AM_IMP}, {"???",AM_IMP},  // 28-2B
    {"BIT",AM_ABS}, {"AND",AM_ABS}, {"ROL",AM_ABS}, {"???",AM_IMP},  // 2C-2F
    {"BMI",AM_REL}, {"AND",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // 30-33
    {"???",AM_IMP}, {"AND",AM_ZPX}, {"ROL",AM_ZPX}, {"???",AM_IMP},  // 34-37
    {"SEC",AM_IMP}, {"AND",AM_ABY}, {"???",AM_IMP}, {"???",AM_IMP},  // 38-3B
    {"???",AM_IMP}, {"AND",AM_ABX}, {"ROL",AM_ABX}, {"???",AM_IMP},  // 3C-3F
    {"RTI",AM_IMP}, {"EOR",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // 40-43
    {"???",AM_IMP}, {"EOR",AM_ZP},  {"LSR",AM_ZP},  {"???",AM_IMP},  // 44-47
    {"PHA",AM_IMP}, {"EOR",AM_IMM}, {"LSR",AM_IMP}, {"???",AM_IMP},  // 48-4B
    {"JMP",AM_ABS}, {"EOR",AM_ABS}, {"LSR",AM_ABS}, {"???",AM_IMP},  // 4C-4F
    {"BVC",AM_REL}, {"EOR",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // 50-53
    {"???",AM_IMP}, {"EOR",AM_ZPX}, {"LSR",AM_ZPX}, {"???",AM_IMP},  // 54-57
    {"CLI",AM_IMP}, {"EOR",AM_ABY}, {"???",AM_IMP}, {"???",AM_IMP},  // 58-5B
    {"???",AM_IMP}, {"EOR",AM_ABX}, {"LSR",AM_ABX}, {"???",AM_IMP},  // 5C-5F
    {"RTS",AM_IMP}, {"ADC",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // 60-63
    {"???",AM_IMP}, {"ADC",AM_ZP},  {"ROR",AM_ZP},  {"???",AM_IMP},  // 64-67
    {"PLA",AM_IMP}, {"ADC",AM_IMM}, {"ROR",AM_IMP}, {"???",AM_IMP},  // 68-6B
    {"JMP",AM_IND}, {"ADC",AM_ABS}, {"ROR",AM_ABS}, {"???",AM_IMP},  // 6C-6F
    {"BVS",AM_REL}, {"ADC",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // 70-73
    {"???",AM_IMP}, {"ADC",AM_ZPX}, {"ROR",AM_ZPX}, {"???",AM_IMP},  // 74-77
    {"SEI",AM_IMP}, {"ADC",AM_ABY}, {"???",AM_IMP}, {"???",AM_IMP},  // 78-7B
    {"???",AM_IMP}, {"ADC",AM_ABX}, {"ROR",AM_ABX}, {"???",AM_IMP},  // 7C-7F
    {"???",AM_IMP}, {"STA",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // 80-83
    {"STY",AM_ZP},  {"STA",AM_ZP},  {"STX",AM_ZP},  {"???",AM_IMP},  // 84-87
    {"DEY",AM_IMP}, {"???",AM_IMP}, {"TXA",AM_IMP}, {"???",AM_IMP},  // 88-8B
    {"STY",AM_ABS}, {"STA",AM_ABS}, {"STX",AM_ABS}, {"???",AM_IMP},  // 8C-8F
    {"BCC",AM_REL}, {"STA",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // 90-93
    {"STY",AM_ZPX}, {"STA",AM_ZPX}, {"STX",AM_ZPY}, {"???",AM_IMP},  // 94-97
    {"TYA",AM_IMP}, {"STA",AM_ABY}, {"TXS",AM_IMP}, {"???",AM_IMP},  // 98-9B
    {"???",AM_IMP}, {"STA",AM_ABX}, {"???",AM_IMP}, {"???",AM_IMP},  // 9C-9F
    {"LDY",AM_IMM}, {"LDA",AM_IZX}, {"LDX",AM_IMM}, {"???",AM_IMP},  // A0-A3
    {"LDY",AM_ZP},  {"LDA",AM_ZP},  {"LDX",AM_ZP},  {"???",AM_IMP},  // A4-A7
    {"TAY",AM_IMP}, {"LDA",AM_IMM}, {"TAX",AM_IMP}, {"???",AM_IMP},  // A8-AB
    {"LDY",AM_ABS}, {"LDA",AM_ABS}, {"LDX",AM_ABS}, {"???",AM_IMP},  // AC-AF
    {"BCS",AM_REL}, {"LDA",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // B0-B3
    {"LDY",AM_ZPX}, {"LDA",AM_ZPX}, {"LDX",AM_ZPY}, {"???",AM_IMP},  // B4-B7
    {"CLV",AM_IMP}, {"LDA",AM_ABY}, {"TSX",AM_IMP}, {"???",AM_IMP},  // B8-BB
    {"LDY",AM_ABX}, {"LDA",AM_ABX}, {"LDX",AM_ABY}, {"???",AM_IMP},  // BC-BF
    {"CPY",AM_IMM}, {"CMP",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // C0-C3
    {"CPY",AM_ZP},  {"CMP",AM_ZP},  {"DEC",AM_ZP},  {"???",AM_IMP},  // C4-C7
    {"INY",AM_IMP}, {"CMP",AM_IMM}, {"DEX",AM_IMP}, {"???",AM_IMP},  // C8-CB
    {"CPY",AM_ABS}, {"CMP",AM_ABS}, {"DEC",AM_ABS}, {"???",AM_IMP},  // CC-CF
    {"BNE",AM_REL}, {"CMP",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // D0-D3
    {"???",AM_IMP}, {"CMP",AM_ZPX}, {"DEC",AM_ZPX}, {"???",AM_IMP},  // D4-D7
    {"CLD",AM_IMP}, {"CMP",AM_ABY}, {"???",AM_IMP}, {"???",AM_IMP},  // D8-DB
    {"???",AM_IMP}, {"CMP",AM_ABX}, {"DEC",AM_ABX}, {"???",AM_IMP},  // DC-DF
    {"CPX",AM_IMM}, {"SBC",AM_IZX}, {"???",AM_IMP}, {"???",AM_IMP},  // E0-E3
    {"CPX",AM_ZP},  {"SBC",AM_ZP},  {"INC",AM_ZP},  {"???",AM_IMP},  // E4-E7
    {"INX",AM_IMP}, {"SBC",AM_IMM}, {"NOP",AM_IMP}, {"???",AM_IMP},  // E8-EB
    {"CPX",AM_ABS}, {"SBC",AM_ABS}, {"INC",AM_ABS}, {"???",AM_IMP},  // EC-EF
    {"BEQ",AM_REL}, {"SBC",AM_IZY}, {"???",AM_IMP}, {"???",AM_IMP},  // F0-F3
    {"???",AM_IMP}, {"SBC",AM_ZPX}, {"INC",AM_ZPX}, {"???",AM_IMP},  // F4-F7
    {"SED",AM_IMP}, {"SBC",AM_ABY}, {"???",AM_IMP}, {"???",AM_IMP},  // F8-FB
    {"???",AM_IMP}, {"SBC",AM_ABX}, {"INC",AM_ABX}, {"???",AM_IMP},  // FC-FF
};

// 65C02 (Rockwell/WDC) overrides for opcodes the NMOS table renders as
// "???". Returns the CMOS info for those opcodes, else the NMOS entry. The
// Rockwell bit-branch ops BBR/BBS are 3 bytes (zp + relative) — the classic
// disassembler desync if treated as 2; modelled as AM_ZPR.
OpcodeInfo cmosInfo(uint8_t op)
{
    switch (op) {
        case 0x04: return {"TSB", AM_ZP};   case 0x0C: return {"TSB", AM_ABS};
        case 0x12: return {"ORA", AM_IZP};  case 0x14: return {"TRB", AM_ZP};
        case 0x1A: return {"INC", AM_ACC};  case 0x1C: return {"TRB", AM_ABS};
        case 0x32: return {"AND", AM_IZP};  case 0x34: return {"BIT", AM_ZPX};
        case 0x3A: return {"DEC", AM_ACC};  case 0x3C: return {"BIT", AM_ABX};
        case 0x52: return {"EOR", AM_IZP};  case 0x5A: return {"PHY", AM_IMP};
        case 0x64: return {"STZ", AM_ZP};   case 0x72: return {"ADC", AM_IZP};
        case 0x74: return {"STZ", AM_ZPX};  case 0x7A: return {"PLY", AM_IMP};
        case 0x7C: return {"JMP", AM_IAX};
        case 0x80: return {"BRA", AM_REL};  case 0x89: return {"BIT", AM_IMM};
        case 0x92: return {"STA", AM_IZP};  case 0x9C: return {"STZ", AM_ABS};
        case 0x9E: return {"STZ", AM_ABX};  case 0xB2: return {"LDA", AM_IZP};
        case 0xCB: return {"WAI", AM_IMP};  case 0xD2: return {"CMP", AM_IZP};
        case 0xDA: return {"PHX", AM_IMP};  case 0xDB: return {"STP", AM_IMP};
        case 0xF2: return {"SBC", AM_IZP};  case 0xFA: return {"PLX", AM_IMP};
        // Rockwell RMB0-7 ($07,$17,…,$77) / SMB0-7 ($87,$97,…,$F7): zp, 2 B.
        case 0x07: return {"RMB0", AM_ZP};  case 0x17: return {"RMB1", AM_ZP};
        case 0x27: return {"RMB2", AM_ZP};  case 0x37: return {"RMB3", AM_ZP};
        case 0x47: return {"RMB4", AM_ZP};  case 0x57: return {"RMB5", AM_ZP};
        case 0x67: return {"RMB6", AM_ZP};  case 0x77: return {"RMB7", AM_ZP};
        case 0x87: return {"SMB0", AM_ZP};  case 0x97: return {"SMB1", AM_ZP};
        case 0xA7: return {"SMB2", AM_ZP};  case 0xB7: return {"SMB3", AM_ZP};
        case 0xC7: return {"SMB4", AM_ZP};  case 0xD7: return {"SMB5", AM_ZP};
        case 0xE7: return {"SMB6", AM_ZP};  case 0xF7: return {"SMB7", AM_ZP};
        // Rockwell BBR0-7 ($0F,$1F,…,$7F) / BBS0-7 ($8F,$9F,…,$FF): zp+rel, 3 B.
        case 0x0F: return {"BBR0", AM_ZPR}; case 0x1F: return {"BBR1", AM_ZPR};
        case 0x2F: return {"BBR2", AM_ZPR}; case 0x3F: return {"BBR3", AM_ZPR};
        case 0x4F: return {"BBR4", AM_ZPR}; case 0x5F: return {"BBR5", AM_ZPR};
        case 0x6F: return {"BBR6", AM_ZPR}; case 0x7F: return {"BBR7", AM_ZPR};
        case 0x8F: return {"BBS0", AM_ZPR}; case 0x9F: return {"BBS1", AM_ZPR};
        case 0xAF: return {"BBS2", AM_ZPR}; case 0xBF: return {"BBS3", AM_ZPR};
        case 0xCF: return {"BBS4", AM_ZPR}; case 0xDF: return {"BBS5", AM_ZPR};
        case 0xEF: return {"BBS6", AM_ZPR}; case 0xFF: return {"BBS7", AM_ZPR};
        // Undocumented multi-byte NOP slots the CMOS core actually executes
        // (UnoffImm/Unoff2/UnoffZpX/Unoff3/UnoffAbs4 in M6502.cpp). Without
        // these they fall through to the 1-byte "???" entry and desync every
        // subsequent disasm line. Lengths mirror the core's PC advance.
        case 0x02: case 0x22: case 0x42: case 0x62:
        case 0x82: case 0xC2: case 0xE2: return {"NOP", AM_IMM};  // 2 B (UnoffImm)
        case 0x44: return {"NOP", AM_ZP};                         // 2 B (Unoff2)
        case 0x54: case 0xD4: case 0xF4: return {"NOP", AM_ZPX};  // 2 B (UnoffZpX)
        case 0x5C: return {"NOP", AM_ABS};                        // 3 B (Unoff3)
        case 0xDC: case 0xFC: return {"NOP", AM_ABX};             // 3 B (UnoffAbs4)
        default:   return opcodeInfo[op];
    }
}

} // namespace

std::string disassemble6502(const uint8_t* mem, uint16_t pc, int& instrLen, bool cmos)
{
    uint8_t opcode = mem[pc];
    const OpcodeInfo info = cmos ? cmosInfo(opcode) : opcodeInfo[opcode];
    uint8_t lo = mem[(pc + 1) & 0xFFFF];
    uint8_t hi = mem[(pc + 2) & 0xFFFF];
    char buf[32];

    switch (info.mode) {
    case AM_IMP:
        instrLen = 1;
        std::snprintf(buf, sizeof(buf), "%s", info.mnemonic);
        break;
    case AM_IMM:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s #$%02X", info.mnemonic, lo);
        break;
    case AM_ZP:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s $%02X", info.mnemonic, lo);
        break;
    case AM_ZPX:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s $%02X,X", info.mnemonic, lo);
        break;
    case AM_ZPY:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s $%02X,Y", info.mnemonic, lo);
        break;
    case AM_ABS:
        instrLen = 3;
        std::snprintf(buf, sizeof(buf), "%s $%04X", info.mnemonic, lo | (hi << 8));
        break;
    case AM_ABX:
        instrLen = 3;
        std::snprintf(buf, sizeof(buf), "%s $%04X,X", info.mnemonic, lo | (hi << 8));
        break;
    case AM_ABY:
        instrLen = 3;
        std::snprintf(buf, sizeof(buf), "%s $%04X,Y", info.mnemonic, lo | (hi << 8));
        break;
    case AM_IND:
        instrLen = 3;
        std::snprintf(buf, sizeof(buf), "%s ($%04X)", info.mnemonic, lo | (hi << 8));
        break;
    case AM_IZX:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s ($%02X,X)", info.mnemonic, lo);
        break;
    case AM_IZY:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s ($%02X),Y", info.mnemonic, lo);
        break;
    case AM_REL: {
        instrLen = 2;
        uint16_t target = pc + 2 + static_cast<int8_t>(lo);
        std::snprintf(buf, sizeof(buf), "%s $%04X", info.mnemonic, target);
        break;
    }
    case AM_ACC:
        instrLen = 1;
        std::snprintf(buf, sizeof(buf), "%s A", info.mnemonic);
        break;
    case AM_IZP:
        instrLen = 2;
        std::snprintf(buf, sizeof(buf), "%s ($%02X)", info.mnemonic, lo);
        break;
    case AM_IAX:
        instrLen = 3;
        std::snprintf(buf, sizeof(buf), "%s ($%04X,X)", info.mnemonic, lo | (hi << 8));
        break;
    case AM_ZPR: {
        // BBR/BBS: zp byte (lo) + relative byte (hi). 3 bytes total; target
        // is relative to the instruction's END (pc + 3).
        instrLen = 3;
        uint16_t target = pc + 3 + static_cast<int8_t>(hi);
        std::snprintf(buf, sizeof(buf), "%s $%02X,$%04X", info.mnemonic, lo, target);
        break;
    }
    }
    return buf;
}

} // namespace pom2

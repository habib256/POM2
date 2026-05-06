// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ProDOSHardDiskCard.h"
#include "Logger.h"

#include <fstream>

namespace {

constexpr uint16_t kDeviceBase = 0xC080 + ProDOSHardDiskCard::kSlot * 16; // $C0D0
constexpr uint8_t  kSlotRomHi  = 0xC0 + ProDOSHardDiskCard::kSlot;        // $C5
constexpr uint8_t  kUnitNumber = ProDOSHardDiskCard::kSlot << 4;          // $50
constexpr uint8_t  kDriverOff  = 0x50;
constexpr uint8_t  kBootOff    = 0x20;

void emit(std::array<uint8_t, 256>& rom, uint8_t& pc, std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[pc++] = b;
}

} // namespace

ProDOSHardDiskCard::ProDOSHardDiskCard()
{
    buildRom();
}

bool ProDOSHardDiskCard::loadImage(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError = "Cannot open HDV image: " + path;
        pom2::log().warn("HDV", lastError);
        return false;
    }

    f.seekg(0, std::ios::end);
    const auto size = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (size == 0 || (size % kBlockBytes) != 0) {
        lastError = "HDV image size is not a whole number of 512-byte blocks: " +
                    std::to_string(size);
        pom2::log().warn("HDV", lastError);
        return false;
    }
    if ((size / kBlockBytes) > 0x10000u) {
        lastError = "HDV image has more than 65536 ProDOS blocks: " +
                    std::to_string(size / kBlockBytes);
        pom2::log().warn("HDV", lastError);
        return false;
    }

    std::vector<uint8_t> bytes(size);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        lastError = "Short read on HDV image: " + path;
        pom2::log().warn("HDV", lastError);
        return false;
    }

    image = std::move(bytes);
    imagePath = path;
    imageLoaded = true;
    selectedBlock = 0;
    streamOffset = 0;

    pom2::log().info("HDV", "Loaded " + path + " (" +
                            std::to_string(getBlockCount()) + " blocks)");
    return true;
}

void ProDOSHardDiskCard::ejectImage()
{
    image.clear();
    imagePath.clear();
    imageLoaded = false;
    selectedBlock = 0;
    streamOffset = 0;
}

void ProDOSHardDiskCard::onReset()
{
    selectedBlock = 0;
    streamOffset = 0;
}

uint8_t ProDOSHardDiskCard::slotRomRead(uint8_t low8)
{
    return rom[low8];
}

void ProDOSHardDiskCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    // Firmware protocol:
    //   $C0D0 write = block low byte
    //   $C0D1 write = block high byte
    //   $C0D2 read  = next byte from selected 512-byte block
    if (low4 == 0x0) {
        selectedBlock = static_cast<uint16_t>((selectedBlock & 0xFF00u) | v);
        streamOffset = 0;
    } else if (low4 == 0x1) {
        selectedBlock = static_cast<uint16_t>((selectedBlock & 0x00FFu) |
                                              (static_cast<uint16_t>(v) << 8));
        streamOffset = 0;
    }
}

uint8_t ProDOSHardDiskCard::deviceSelectRead(uint8_t low4)
{
    if (low4 == 0x2) return readDataByte();
    if (low4 == 0x3) return imageLoaded ? 0x00 : 0x80;
    return 0;
}

uint8_t ProDOSHardDiskCard::readDataByte()
{
    if (!imageLoaded) return 0xFF;

    const size_t absolute = static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    const uint8_t out = (absolute < image.size()) ? image[absolute] : 0xFF;
    streamOffset = (streamOffset + 1) % kBlockBytes;
    return out;
}

void ProDOSHardDiskCard::buildRom()
{
    rom.fill(0xEA); // NOP padding

    // Entry used by PR#5 / direct boot: load block 0 at $0800, then jump to
    // the universal ProDOS boot loader's real entry at $0801 with X=unit.
    rom[0x00] = 0x4C;        // JMP $C520
    rom[0x01] = kBootOff;    // ProDOS signature byte: $Cn01 = $20
    rom[0x02] = kSlotRomHi;
    rom[0x03] = 0x00;        // ProDOS signature byte: $Cn03 = $00
    rom[0x05] = 0x03;        // ProDOS signature byte: $Cn05 = $03
    rom[0x07] = 0x01;        // non-zero: plain ProDOS block device, not SmartPort
    rom[0xFE] = 0x03;        // read/write/status flags; high nibble = one fixed unit
    rom[0xFF] = kDriverOff;  // ProDOS driver entry offset

    uint8_t pc = kBootOff;
    emit(rom, pc, {
        0xA9, 0x01,       // LDA #$01        ; read command
        0x85, 0x42,       // STA $42
        0xA9, kUnitNumber,
        0x85, 0x43,       // STA $43         ; slot 5, drive 1
        0xA9, 0x00,
        0x85, 0x44,       // STA $44         ; buffer low = $00
        0xA9, 0x08,
        0x85, 0x45,       // STA $45         ; buffer high = $08
        0xA9, 0x00,
        0x85, 0x46,       // STA $46         ; block low = 0
        0x85, 0x47,       // STA $47         ; block high = 0
        0x20, kDriverOff, kSlotRomHi, // JSR $C550
        0xB0, 0x07,       // BCS error
        0xA2, kUnitNumber,// LDX #unit
        0xA9, 0x00,       // LDA #$00
        0x4C, 0x01, 0x08, // JMP $0801
        0x4C, 0x42, kSlotRomHi // error: JMP error
    });

    pc = kDriverOff;
    emit(rom, pc, {
        0xA5, 0x42,       // LDA $42         ; command
        0xC9, 0x01,       // CMP #$01
        0xF0, 0x0C,       // BEQ read
        0xC9, 0x00,       // CMP #$00
        0xF0, 0x04,       // BEQ status
        0xA9, 0x2B,       // LDA #$2B        ; write-protected / unsupported write
        0x38,             // SEC
        0x60,             // RTS
        0xA9, 0x00,       // status: LDA #$00
        0x18,             // CLC
        0x60              // RTS
    });

    const uint8_t dataReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    emit(rom, pc, {
        0xA5, 0x46,       // read: LDA $46   ; block low
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
        0xA5, 0x47,       // LDA $47         ; block high
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x01), 0xC0,
        0xA0, 0x00,       // LDY #$00
        0xAD, dataReg, 0xC0, // page 1: LDA $C0D2
        0x91, 0x44,       // STA ($44),Y
        0xC8,             // INY
        0xD0, 0xF8,       // BNE page 1
        0xE6, 0x45,       // INC $45
        0xAD, dataReg, 0xC0, // page 2: LDA $C0D2
        0x91, 0x44,       // STA ($44),Y
        0xC8,             // INY
        0xD0, 0xF8,       // BNE page 2
        0xC6, 0x45,       // DEC $45
        0x18,             // CLC
        0x60              // RTS
    });
}

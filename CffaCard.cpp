// POM2 Apple II Emulator
// Copyright (C) 2026

#include "CffaCard.h"
#include "Logger.h"

#include <fstream>

namespace pom2 {

bool CffaCard::loadRom(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError_ = "Cannot open CFFA ROM: " + path;
        pom2::log().warn("CFFA", lastError_);
        return false;
    }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (sz != kRomBytes) {
        lastError_ = "CFFA ROM must be exactly 4096 bytes (got " +
                     std::to_string(sz) + "): " + path;
        pom2::log().warn("CFFA", lastError_);
        return false;
    }
    f.read(reinterpret_cast<char*>(rom_.data()),
           static_cast<std::streamsize>(rom_.size()));
    if (!f) {
        lastError_ = "Short read on CFFA ROM: " + path;
        pom2::log().warn("CFFA", lastError_);
        return false;
    }
    romLoaded_ = true;
    pom2::log().info("CFFA", "Loaded firmware ROM: " + path);
    return true;
}

bool CffaCard::loadImage(const std::string& path)
{
    const bool ok = ata_.backing().loadImage(path);
    ata_.reset();
    if (!ok) lastError_ = ata_.backing().lastError();
    return ok;
}

bool CffaCard::loadImageFromBytes(std::vector<uint8_t> bytes,
                                  const std::string& label,
                                  const std::string& hostFolder)
{
    const bool ok = ata_.backing().loadFromBytes(std::move(bytes), label, hostFolder);
    ata_.reset();
    if (!ok) lastError_ = ata_.backing().lastError();
    return ok;
}

void CffaCard::ejectImage()
{
    // Save-on-eject when the user opted into write-back and the medium allows.
    Block512Backing& b = ata_.backing();
    if (b.isLoaded() && b.hasUnsavedChanges() &&
        b.isWriteBackEnabled() && !b.isWriteProtected()) {
        if (!b.saveDirty())
            pom2::log().warn("CFFA", "Save-on-eject failed: " + b.lastError());
    }
    b.eject();
    ata_.reset();
}

void CffaCard::onReset()
{
    ata_.reset();
    lastReadData_  = 0;
    lastWriteData_ = 0;
}

// ── $C0nX device-select — MAME a2cffa.cpp read_c0nx (~L117-147) ────────────
uint8_t CffaCard::deviceSelectRead(uint8_t low4)
{
    switch (low4) {
        case 0x0: // high byte of the last 16-bit ATA data word read at $C0n8
            return static_cast<uint8_t>(lastReadData_ >> 8);
        case 0x3: // EEPROM write-enable off
            writeProtect_ = false;
            return 0x00;
        case 0x4: // EEPROM write-enable on (protect)
            writeProtect_ = true;
            return 0x00;
        case 0x8: { // ATA data register: 16-bit read, low byte to bus, high latched
            const uint16_t d = ata_.cs0_r(0);
            lastReadData_ = d;
            return static_cast<uint8_t>(d & 0xFF);
        }
        case 0x9: case 0xA: case 0xB: case 0xC:
        case 0xD: case 0xE: case 0xF: // ATA taskfile registers 1..7
            return static_cast<uint8_t>(ata_.cs0_r(static_cast<uint8_t>(low4 - 8)) & 0xFF);
        default:
            return 0x00;
    }
}

// ── $C0nX device-select — MAME a2cffa.cpp write_c0nx (~L154-176) ───────────
void CffaCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
        case 0x0: // high byte of the 16-bit ATA data word (committed at $C0n8)
            lastWriteData_ = static_cast<uint16_t>((lastWriteData_ & 0x00FF) |
                                                   (static_cast<uint16_t>(v) << 8));
            break;
        case 0x3: writeProtect_ = false; break;
        case 0x4: writeProtect_ = true;  break;
        case 0x8: // ATA data register: combine latched high byte, write 16-bit
            lastWriteData_ = static_cast<uint16_t>((lastWriteData_ & 0xFF00) | v);
            ata_.cs0_w(0, lastWriteData_);
            break;
        case 0x9: case 0xA: case 0xB: case 0xC:
        case 0xD: case 0xE: case 0xF: // ATA taskfile registers 1..7
            ata_.cs0_w(static_cast<uint8_t>(low4 - 8), v);
            break;
        default:
            break;
    }
}

// ── $CnXX slot ROM — MAME a2cffa.cpp read_cnxx (~L179-183) ─────────────────
uint8_t CffaCard::slotRomRead(uint8_t low8)
{
    return rom_[static_cast<size_t>(low8) + static_cast<size_t>(slot_) * 0x100];
}

// ── $C800 expansion ROM — MAME a2cffa.cpp read_c800 (~L185-191) ────────────
uint8_t CffaCard::expansionRomRead(uint16_t offset)
{
    const size_t idx = 0x800 + offset;
    return (idx < rom_.size()) ? rom_[idx] : 0xFF;
}

void CffaCard::expansionRomWrite(uint16_t offset, uint8_t v)
{
    if (writeProtect_) return;          // EEPROM write-enable gate
    const size_t idx = 0x800 + offset;
    if (idx < rom_.size()) rom_[idx] = v;
}

} // namespace pom2

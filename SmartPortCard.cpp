// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SmartPortCard.h"

#include "FloppySoundSink.h"
#include "Logger.h"

#include <cstring>

namespace pom2 {

namespace {

constexpr uint8_t kBootOff   = 0x20;
constexpr uint8_t kDriverOff = 0x50;

void emit(std::array<uint8_t, 256>& rom, uint8_t& pc,
          std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[pc++] = b;
}

} // anon namespace

SmartPortCard::SmartPortCard(int slot)
    : slot_(slot)
{
    selectedBlock_.fill(0);
    streamOffset_.fill(0);
    readCacheValid_.fill(false);
    readCacheBlock_.fill(0xFFFF);
    writeBufPrimed_.fill(false);
    buildRom();
}

std::unique_ptr<SmartPortUnit>
SmartPortCard::setUnit(size_t idx, std::unique_ptr<SmartPortUnit> u)
{
    if (idx >= kMaxUnits) return u;     // index out of range — return back
    // Reset any in-flight transfer state for this slot so the new unit
    // doesn't inherit a half-streamed block from the old one.
    selectedBlock_[idx]  = 0;
    streamOffset_[idx]   = 0;
    readCacheValid_[idx] = false;
    readCacheBlock_[idx] = 0xFFFF;
    writeBufPrimed_[idx] = false;
    auto old = std::move(units_[idx]);
    units_[idx] = std::move(u);
    return old;
}

void SmartPortCard::onReset()
{
    activeUnit_ = 0;
    selectedBlock_.fill(0);
    streamOffset_.fill(0);
    readCacheValid_.fill(false);
    readCacheBlock_.fill(0xFFFF);
    writeBufPrimed_.fill(false);
    if (audibleMotorOn_ && sound_) sound_->motor(false, true);
    audibleMotorOn_ = false;
    lastAccessCycle_ = 0;
}

void SmartPortCard::advanceCycles(int cycles)
{
    cpuCycleTotal_ += static_cast<uint64_t>(cycles);
    if (audibleMotorOn_ &&
        cpuCycleTotal_ - lastAccessCycle_ > kSpinDownCycles) {
        if (sound_) sound_->motor(false, true);
        audibleMotorOn_ = false;
    }
}

void SmartPortCard::noteAccess()
{
    if (!sound_) return;
    if (!audibleMotorOn_) {
        sound_->motor(true, true);
        audibleMotorOn_ = true;
    }
    // One step event per accessed block. The sound device classifies
    // gap in emulated CPU cycles, so back-to-back ProDOS block reads
    // (~tens of ms apart at native speed) land in the seek-rate band
    // and the user hears a continuous seek; isolated accesses sound
    // like a single click.
    sound_->step(static_cast<int>(selectedBlock_[activeUnit_]),
                 cpuCycleTotal_);
    lastAccessCycle_ = cpuCycleTotal_;
}

uint8_t SmartPortCard::slotRomRead(uint8_t low8)
{
    return rom_[low8];
}

uint8_t SmartPortCard::deviceSelectRead(uint8_t low4)
{
    switch (low4) {
        case 0x3: return readDataByte();
        case 0x4: return statusByte();
        default:  return 0xFF;
    }
}

void SmartPortCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
        case 0x0: {                             // drive / unit select
            const size_t u = static_cast<size_t>(v) & (kMaxUnits - 1);
            activeUnit_ = u;
            streamOffset_[u] = 0;
            break;
        }
        case 0x1: {                             // block LO of active unit
            const size_t u = activeUnit_;
            selectedBlock_[u] = static_cast<uint16_t>(
                (selectedBlock_[u] & 0xFF00u) | v);
            streamOffset_[u]   = 0;
            readCacheValid_[u] = false;
            writeBufPrimed_[u] = false;
            break;
        }
        case 0x2: {                             // block HI of active unit
            const size_t u = activeUnit_;
            selectedBlock_[u] = static_cast<uint16_t>(
                (selectedBlock_[u] & 0x00FFu) |
                (static_cast<uint16_t>(v) << 8));
            streamOffset_[u]   = 0;
            readCacheValid_[u] = false;
            writeBufPrimed_[u] = false;
            break;
        }
        case 0x3:                               // streaming write
            writeDataByte(v);
            break;
        default:
            break;
    }
}

uint8_t SmartPortCard::statusByte() const
{
    const SmartPortUnit* u = (activeUnit_ < kMaxUnits)
        ? units_[activeUnit_].get() : nullptr;
    uint8_t s = (u && u->isLoaded()) ? 0x00 : 0x80;
    if (!u || u->isWriteProtected()) s |= 0x40;
    return s;
}

uint8_t SmartPortCard::readDataByte()
{
    const size_t u = activeUnit_;
    SmartPortUnit* unit = units_[u].get();
    if (!unit || !unit->isLoaded()) return 0xFF;

    // Lazy per-unit read cache. The driver issues 512 byte-reads per
    // ProDOS block; we hit the underlying SmartPortUnit::readBlock once
    // when streamOffset_ wraps to 0 (or the cached block doesn't match)
    // and return cached bytes for the remaining 511.
    if (streamOffset_[u] == 0 ||
        !readCacheValid_[u] ||
        readCacheBlock_[u] != selectedBlock_[u])
    {
        if (!unit->readBlock(selectedBlock_[u], readCache_[u].data())) {
            return 0xFF;
        }
        readCacheBlock_[u] = selectedBlock_[u];
        readCacheValid_[u] = true;
        noteAccess();
    }
    const uint8_t out = readCache_[u][streamOffset_[u]];
    streamOffset_[u] = (streamOffset_[u] + 1) % kBlockBytes;
    return out;
}

void SmartPortCard::writeDataByte(uint8_t v)
{
    const size_t u = activeUnit_;
    SmartPortUnit* unit = units_[u].get();
    if (!unit || !unit->isLoaded() || unit->isWriteProtected()) return;

    // Mirror the read cache for writes: accumulate 512 bytes in
    // `writeBuf_[u]`, commit when streamOffset_ wraps back to 0.
    if (streamOffset_[u] == 0 && !writeBufPrimed_[u]) {
        // Partial-block update: pre-fill with existing contents so the
        // un-written tail isn't zeroed.
        if (!unit->readBlock(selectedBlock_[u], writeBuf_[u].data())) {
            std::memset(writeBuf_[u].data(), 0, kBlockBytes);
        }
        writeBufPrimed_[u] = true;
    }
    writeBuf_[u][streamOffset_[u]] = v;
    streamOffset_[u] = (streamOffset_[u] + 1) % kBlockBytes;
    if (streamOffset_[u] == 0) {
        (void)unit->writeBlock(selectedBlock_[u], writeBuf_[u].data());
        writeBufPrimed_[u] = false;
        // The just-committed block is no longer the most-recently-read
        // one; invalidate the read cache so the next read pulls fresh.
        readCacheValid_[u] = false;
        noteAccess();
    }
}

void SmartPortCard::buildRom()
{
    rom_.fill(0xEA);                            // NOP padding

    const uint16_t kDeviceBase = static_cast<uint16_t>(0xC080 + slot_ * 16);
    const uint8_t  kSlotRomHi  = static_cast<uint8_t>(0xC0 + slot_);
    const uint8_t  kUnitDrv1   = static_cast<uint8_t>(slot_ << 4);

    // ── ProDOS / SmartPort signature ───────────────────────────────────
    rom_[0x00] = 0x4C;                          // JMP $Cn20
    rom_[0x01] = kBootOff;
    rom_[0x02] = kSlotRomHi;
    rom_[0x03] = 0x00;
    rom_[0x05] = 0x03;
    rom_[0x07] = 0x3C;                          // SmartPort signature
    rom_[0xFE] = 0x13;                          // read+write+status, 2 units
    rom_[0xFF] = kDriverOff;

    // ── Boot routine ($Cn20) ───────────────────────────────────────────
    uint8_t pc = kBootOff;
    emit(rom_, pc, {
        0xA9, 0x01,            // LDA #$01           ; ProDOS read cmd
        0x85, 0x42,
        0xA9, kUnitDrv1,       // unit = slot×16, drive 1
        0x85, 0x43,
        0xA9, 0x00,
        0x85, 0x44,            // buffer LO = $00
        0xA9, 0x08,
        0x85, 0x45,            // buffer HI = $08
        0xA9, 0x00,
        0x85, 0x46,            // block LO = 0
        0x85, 0x47,            // block HI = 0
        0x20, kDriverOff, kSlotRomHi, // JSR $Cn50 (driver)
        0xB0, 0x07,            // BCS  error
        0xA2, kUnitDrv1,       // X = unit
        0xA9, 0x00,
        0x4C, 0x01, 0x08,      // JMP $0801
        0x4C, 0xE0, kSlotRomHi // error: JMP $CnE0 (halt loop)
    });

    rom_[0xE0] = 0x4C;
    rom_[0xE1] = 0xE0;
    rom_[0xE2] = kSlotRomHi;

    // ── ProDOS driver dispatch ($Cn50) ─────────────────────────────────
    // ProDOS calls here with $42 = command, $43 = unit, $44/$45 = buffer,
    // $46/$47 = block. Unit byte bit 7 = drive (0 = drive 1, 1 = drive 2).
    pc = kDriverOff;
    emit(rom_, pc, {
        0xA5, 0x43,            // LDA $43           ; unit byte
        0x0A,                  // ASL A             ; bit7 → carry
        0xA9, 0x00,
        0x2A,                  // ROL A             ; A = drive (0 or 1)
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
                               // STA $C0n0         ; latch unit
        0xA5, 0x42,            // LDA $42           ; command
        0xC9, 0x01,            // CMP #$01
        0xF0, 0x10,            // BEQ read   (+16)
        0xC9, 0x02,            // CMP #$02
        0xF0, 0x22,            // BEQ write  (+34)
        0xC9, 0x00,            // CMP #$00
        0xF0, 0x04,            // BEQ status (+4)
        0xA9, 0x01,            // bad cmd: LDA #$01
        0x38,                  // SEC
        0x60,                  // RTS
        0xA9, 0x00,            // status: LDA #$00
        0x18,                  // CLC
        0x60                   // RTS
    });

    const uint8_t blkLoReg = static_cast<uint8_t>(kDeviceBase + 0x01);
    const uint8_t blkHiReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t dataReg  = static_cast<uint8_t>(kDeviceBase + 0x03);
    const uint8_t statReg  = static_cast<uint8_t>(kDeviceBase + 0x04);

    // ── Read block ─────────────────────────────────────────────────────
    emit(rom_, pc, {
        0xA5, 0x46,            // LDA $46
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,
        0xAD, dataReg, 0xC0,
        0x91, 0x44,
        0xC8,
        0xD0, 0xF8,
        0xE6, 0x45,
        0xAD, dataReg, 0xC0,
        0x91, 0x44,
        0xC8,
        0xD0, 0xF8,
        0xC6, 0x45,
        0x18,
        0x60
    });

    // ── Write block ────────────────────────────────────────────────────
    emit(rom_, pc, {
        0xAD, statReg, 0xC0,
        0x29, 0x40,
        0xF0, 0x04,
        0xA9, 0x2B,            // write-protected error
        0x38,
        0x60,
        0xA5, 0x46,
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,
        0xB1, 0x44,
        0x8D, dataReg, 0xC0,
        0xC8,
        0xD0, 0xF8,
        0xE6, 0x45,
        0xB1, 0x44,
        0x8D, dataReg, 0xC0,
        0xC8,
        0xD0, 0xF8,
        0xC6, 0x45,
        0xA9, 0x00,
        0x18,
        0x60
    });
}

} // namespace pom2

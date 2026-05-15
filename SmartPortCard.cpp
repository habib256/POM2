// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SmartPortCard.h"

#include "Disk35Image.h"
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

SmartPortCard::SmartPortCard(int slot, Disk35Image* image0, Disk35Image* image1)
    : slot_(slot), image0_(image0), image1_(image1)
{
    buildRom();
}

void SmartPortCard::onReset()
{
    activeDrive_     = 0;
    selectedBlock_[0] = selectedBlock_[1] = 0;
    streamOffset_ [0] = streamOffset_ [1] = 0;
}

uint8_t SmartPortCard::slotRomRead(uint8_t low8)
{
    return rom_[low8];
}

uint8_t SmartPortCard::deviceSelectRead(uint8_t low4)
{
    switch (low4) {
        case 0x3: return readDataByte();        // streaming read from active drive
        case 0x4: return statusByte();          // status of active drive
        default:  return 0xFF;
    }
}

void SmartPortCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
        case 0x0:                               // drive select
            activeDrive_ = (v & 1);
            streamOffset_[activeDrive_] = 0;
            break;
        case 0x1:                               // block LO of active drive
            selectedBlock_[activeDrive_] =
                static_cast<uint16_t>(
                    (selectedBlock_[activeDrive_] & 0xFF00u) | v);
            streamOffset_[activeDrive_] = 0;
            break;
        case 0x2:                               // block HI of active drive
            selectedBlock_[activeDrive_] =
                static_cast<uint16_t>(
                    (selectedBlock_[activeDrive_] & 0x00FFu) |
                    (static_cast<uint16_t>(v) << 8));
            streamOffset_[activeDrive_] = 0;
            break;
        case 0x3:                               // streaming write into active drive
            writeDataByte(v);
            break;
        default:
            break;
    }
}

uint8_t SmartPortCard::statusByte() const
{
    Disk35Image* img = active();
    uint8_t s = (img && img->isLoaded()) ? 0x00 : 0x80;
    if (!img || img->isWriteProtected()) s |= 0x40;
    return s;
}

uint8_t SmartPortCard::readDataByte()
{
    Disk35Image* img = active();
    if (!img || !img->isLoaded()) return 0xFF;

    // Lazy per-block read cache. We don't want to call readBlock() 512
    // times per ProDOS block (one per byte) — read the block once when
    // streamOffset_ wraps to 0, then return bytes from the cached copy.
    // Two caches so drive 1 / drive 2 don't fight when ProDOS interleaves
    // accesses (rare but legal — driver code below always issues full
    // 512-byte runs back-to-back, so even a single cache would do).
    static thread_local uint8_t  cache[2][kBlockBytes]{};
    static thread_local uint16_t cachedBlock[2] = { 0xFFFF, 0xFFFF };

    const int drv = activeDrive_;
    if (streamOffset_[drv] == 0 || cachedBlock[drv] != selectedBlock_[drv]) {
        if (!img->readBlock(selectedBlock_[drv], cache[drv])) {
            // Block out of range → return $FF and don't advance the stream.
            return 0xFF;
        }
        cachedBlock[drv] = selectedBlock_[drv];
    }
    const uint8_t out = cache[drv][streamOffset_[drv]];
    streamOffset_[drv] = (streamOffset_[drv] + 1) % kBlockBytes;
    return out;
}

void SmartPortCard::writeDataByte(uint8_t v)
{
    Disk35Image* img = active();
    if (!img || !img->isLoaded() || img->isWriteProtected()) return;

    // Mirror the read-side cache for the write path: accumulate 512 bytes
    // in a scratch buffer, then commit to the image when streamOffset_
    // wraps back to 0 (i.e. the 513th write completes the block).
    static thread_local uint8_t  buf[2][kBlockBytes]{};
    static thread_local bool     primed[2] = { false, false };

    const int drv = activeDrive_;
    if (streamOffset_[drv] == 0 && !primed[drv]) {
        // First byte of a fresh block — pre-fill with the existing image
        // contents so a partial-block update (rare but valid) doesn't
        // zero the un-touched tail.
        if (!img->readBlock(selectedBlock_[drv], buf[drv])) {
            std::memset(buf[drv], 0, kBlockBytes);
        }
        primed[drv] = true;
    }
    buf[drv][streamOffset_[drv]] = v;
    streamOffset_[drv] = (streamOffset_[drv] + 1) % kBlockBytes;
    if (streamOffset_[drv] == 0) {
        // Block boundary — commit. saveDirty on eject persists to file.
        (void)img->writeBlock(selectedBlock_[drv], buf[drv]);
        primed[drv] = false;
    }
}

void SmartPortCard::buildRom()
{
    rom_.fill(0xEA);                            // NOP padding

    const uint16_t kDeviceBase = static_cast<uint16_t>(0xC080 + slot_ * 16);
    const uint8_t  kSlotRomHi  = static_cast<uint8_t>(0xC0 + slot_);
    const uint8_t  kUnitDrv1   = static_cast<uint8_t>(slot_ << 4);

    // ── ProDOS / SmartPort signature ───────────────────────────────────
    // Boot vector at $Cn00 jumps to $Cn20 (where the boot routine sits).
    rom_[0x00] = 0x4C;                          // JMP $Cn20
    rom_[0x01] = kBootOff;                      // ProDOS signature $20
    rom_[0x02] = kSlotRomHi;
    rom_[0x03] = 0x00;                          // ProDOS signature $00
    rom_[0x05] = 0x03;                          // ProDOS signature $03
    rom_[0x07] = 0x3C;                          // SmartPort signature
                                                // (non-zero ≠ $01 = block dev
                                                //  with extended unit count)
    rom_[0xFE] = 0x13;                          // bits: status(0)+read(1)
                                                // +write(0=$04 unset for now)
                                                // +units=1 additional (bit4)
    rom_[0xFF] = kDriverOff;                    // driver entry at $Cn50

    // ── Boot routine ($Cn20) ───────────────────────────────────────────
    // PR#n / boot from this slot lands here. Reads block 0 of drive 1
    // into $0800 then jumps to $0801 (standard ProDOS bootstrap).
    uint8_t pc = kBootOff;
    emit(rom_, pc, {
        0xA9, 0x01,            // LDA #$01           ; ProDOS read cmd
        0x85, 0x42,            // STA $42
        0xA9, kUnitDrv1,       // unit = slot×16, drive 1
        0x85, 0x43,            // STA $43
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
        0xA9, 0x00,            // A = 0
        0x4C, 0x01, 0x08,      // JMP $0801
        0x4C, 0xE0, kSlotRomHi // error: JMP $CnE0 (halt loop)
    });

    // Boot error handler — tight loop. Mirrors HDV card.
    rom_[0xE0] = 0x4C;
    rom_[0xE1] = 0xE0;
    rom_[0xE2] = kSlotRomHi;

    // ── ProDOS driver dispatch ($Cn50) ─────────────────────────────────
    // ProDOS calls here with $42 = command, $43 = unit, $44/$45 = buffer,
    // $46/$47 = block. Unit byte bit 7 = drive (0 = drive 1, 1 = drive 2).
    // We first latch the drive via $C0n0, then run the appropriate
    // read / write loop. Layout:
    //
    //   $Cn50..$Cn69  dispatch (26 B): drive-select + cmd switch
    //   $Cn6A..$Cn87  read block  (~30 B)
    //   $Cn88..$CnB7  write block (~48 B)
    pc = kDriverOff;

    // Drive select: A := ($43 >> 7), STA $C0n0, fall through to cmd switch.
    emit(rom_, pc, {
        0xA5, 0x43,            // LDA $43           ; unit byte
        0x0A,                  // ASL A             ; bit7 → carry
        0xA9, 0x00,            // LDA #$00
        0x2A,                  // ROL A             ; A = drive (0 or 1)
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
                               //                   ; latch drive
        0xA5, 0x42,            // LDA $42           ; command
        0xC9, 0x01,            // CMP #$01
        0xF0, 0x10,            // BEQ read   (+16 → read entry)
        0xC9, 0x02,            // CMP #$02
        0xF0, 0x22,            // BEQ write  (+34 → write entry)
        0xC9, 0x00,            // CMP #$00
        0xF0, 0x04,            // BEQ status (+4)
        0xA9, 0x01,            // LDA #$01          ; bad-command error
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
        0xA5, 0x46,            // LDA $46         ; block LO
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47         ; block HI
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,            // LDY #0
        0xAD, dataReg, 0xC0,   // page-1: LDA $C0n3
        0x91, 0x44,            // STA ($44),Y
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE page-1
        0xE6, 0x45,            // INC $45
        0xAD, dataReg, 0xC0,   // page-2: LDA $C0n3
        0x91, 0x44,            // STA ($44),Y
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE page-2
        0xC6, 0x45,            // DEC $45
        0x18,                  // CLC
        0x60                   // RTS
    });

    // ── Write block ────────────────────────────────────────────────────
    emit(rom_, pc, {
        0xAD, statReg, 0xC0,   // LDA status
        0x29, 0x40,            // AND #$40        ; WP bit
        0xF0, 0x04,            // BEQ ok
        0xA9, 0x2B,            // LDA #$2B        ; write-protected
        0x38,                  // SEC
        0x60,                  // RTS
        0xA5, 0x46,            // ok: LDA $46
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,            // LDY #0
        0xB1, 0x44,            // page-1: LDA ($44),Y
        0x8D, dataReg, 0xC0,   // STA $C0n3
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE page-1
        0xE6, 0x45,            // INC $45
        0xB1, 0x44,            // page-2: LDA ($44),Y
        0x8D, dataReg, 0xC0,   // STA $C0n3
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE page-2
        0xC6, 0x45,            // DEC $45
        0xA9, 0x00,
        0x18,                  // CLC
        0x60                   // RTS
    });
}

} // namespace pom2

// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "SmartPortCard.h"

#include "FloppySoundSink.h"
#include "Logger.h"
#include "SmartPort35Unit.h"
#include "SmartPortHdvUnit.h"

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

// $C0n0 unit-select maps the written value with `% kMaxUnits` (deviceSelectWrite
// case 0x0); that is only well-defined for a non-zero unit count.
static_assert(SmartPortCard::kMaxUnits >= 1, "kMaxUnits must be >= 1");

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
    ioError_.fill(false);
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

bool SmartPortCard::exposesIicOnboardRom() const
{
    // //c-class memory map masks all slot ROM behind the forced INTCXROM;
    // Memory punches a hole for this card's $Cn00 firmware ONLY while a unit
    // holds media, so the //c autostart never JMPs into an empty SmartPort.
    // See SlotPeripheral::exposesIicOnboardRom + project_iic_smartport_boot.
    for (const auto& u : units_)
        if (u && u->isLoaded()) return true;
    return false;
}

uint8_t SmartPortCard::deviceSelectRead(uint8_t low4)
{
    switch (low4) {
        case 0x3: return readDataByte();
        case 0x4: return statusByte();
        case 0x5: return blockCountByte(0);   // STATUS block count, low
        case 0x6: return blockCountByte(1);   // STATUS block count, high
        default:  return 0xFF;
    }
}

uint8_t SmartPortCard::blockCountByte(int which) const
{
    // The ProDOS STATUS driver call (cmd $00) must return the device's total
    // block count in X (low) / Y (high). The ROM status routine reads it from
    // these two registers. A driver that left X/Y unset returned garbage,
    // which crashed a ProDOS volume scanner (e.g. BITSY) that enumerated this
    // device after booting from another slot — the //c "Disk II + on-board
    // SmartPort" garble (see project_iic_smartport_boot).
    const SmartPortUnit* u =
        (activeUnit_ < kMaxUnits) ? units_[activeUnit_].get() : nullptr;
    const uint32_t blocks = (u && u->isLoaded()) ? u->blockCount() : 0u;
    return static_cast<uint8_t>((blocks >> (which ? 8 : 0)) & 0xFF);
}

void SmartPortCard::deviceSelectWrite(uint8_t low4, uint8_t v)
{
    switch (low4) {
        case 0x0: {                             // drive / unit select
            // Modulo (not a bitmask) so the mapping stays correct if kMaxUnits
            // is ever raised to a non-power-of-two for extended SmartPort.
            const size_t u = static_cast<size_t>(v) % kMaxUnits;
            activeUnit_ = u;
            // A unit-select starts a fresh transfer: drop any half-streamed
            // write buffer / stale read cache / error so the next op is clean.
            streamOffset_[u]   = 0;
            writeBufPrimed_[u] = false;
            readCacheValid_[u] = false;
            ioError_[u]        = false;
            break;
        }
        case 0x1: {                             // block LO of active unit
            const size_t u = activeUnit_;
            selectedBlock_[u] = static_cast<uint16_t>(
                (selectedBlock_[u] & 0xFF00u) | v);
            streamOffset_[u]   = 0;
            readCacheValid_[u] = false;
            writeBufPrimed_[u] = false;
            ioError_[u]        = false;
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
            ioError_[u]        = false;
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
    if (activeUnit_ < kMaxUnits && ioError_[activeUnit_]) s |= 0x01;  // I/O error
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
            // Out-of-range / failed read: latch an I/O error so the ROM read
            // routine returns carry-set (ProDOS $27) instead of CLC "success"
            // with a garbage buffer, AND keep the byte stream in phase by
            // serving a 0xFF-filled cache — a mid-transfer failure must not
            // desync the remaining 511 reads of the block.
            ioError_[u] = true;
            readCache_[u].fill(0xFF);
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
    if (!unit || !unit->isLoaded()) return;
    if (unit->isWriteProtected()) { ioError_[u] = true; return; }  // surface WP

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
        if (!unit->writeBlock(selectedBlock_[u], writeBuf_[u].data())) {
            // Out-of-range / rejected commit → report failure to ProDOS
            // (ROM write routine tests $C0n4 bit 0 and returns carry-set).
            ioError_[u] = true;
        }
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
    // $Cn07 identifies the controller class to the //c-class boot firmware.
    // $3C = Disk II (the //c internal $C607!) — claiming it makes a //c see
    // TWO Disk II controllers (slot 5 + the internal slot 6) and its boot
    // scan corrupts. $00 = SmartPort, which triggers a SmartPort enumeration
    // this block-only stub can't service. $01 = plain ProDOS block device
    // (non-removable, like ProDOSHardDiskCard) → the //c boots it via the
    // standard JMP $Cn00 path with no Disk II / SmartPort confusion, and //e
    // boot (bootFromSlot, ProDOS via $CnFF) is unaffected. See
    // project_iic_smartport_boot.
    rom_[0x07] = 0x01;                          // ProDOS block device
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
        0xF0, 0x39,            // BEQ write  (+57: skip the 45-byte read block)
        0xC9, 0x00,            // CMP #$00
        0xF0, 0x04,            // BEQ status (+4)
        0xA9, 0x01,            // bad cmd: LDA #$01
        0x38,                  // SEC
        0x60,                  // RTS
        // status: jump to the full STATUS routine at $CnC0 (returns the
        // block count in X/Y). Kept 4 bytes (JMP + NOP pad) so the BEQ
        // read/write offsets above stay valid — pinned by
        // tests/smartport_write_dispatch_test.cpp.
        0x4C, 0xC0, kSlotRomHi, // status: JMP $CnC0
        0xEA                    // pad
    });

    // ── STATUS routine ($CnC0) ─────────────────────────────────────────
    // ProDOS STATUS (cmd $00) must return total blocks in X (low) / Y (high)
    // so a volume scanner (BITSY, ProDOS ONLINE) can size the device. The
    // count comes from $C0n5/$C0n6 (deviceSelectRead 0x5/0x6). The unit was
    // already latched via $C0n0 at the top of the dispatch.
    {
        uint8_t sp = 0xC0;
        emit(rom_, sp, {
            0xAE, static_cast<uint8_t>(kDeviceBase + 0x05), 0xC0, // LDX $C0n5
            0xAC, static_cast<uint8_t>(kDeviceBase + 0x06), 0xC0, // LDY $C0n6
            0xA9, 0x00,        // LDA #$00
            0x18,              // CLC
            0x60               // RTS
        });
    }

    const uint8_t blkLoReg = static_cast<uint8_t>(kDeviceBase + 0x01);
    const uint8_t blkHiReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t dataReg  = static_cast<uint8_t>(kDeviceBase + 0x03);
    const uint8_t statReg  = static_cast<uint8_t>(kDeviceBase + 0x04);

    // ── Read block (45 bytes) ──────────────────────────────────────────
    // Streams 512 bytes, then tests $C0n4 bit 0 (I/O error) so a failed /
    // out-of-range readBlock returns carry-set (ProDOS $27) rather than CLC
    // "success" over a 0xFF-filled buffer. Lengthening this block past the
    // original 34 bytes is why the dispatch `BEQ write` operand above is $39
    // (was $2E) — pinned by tests/smartport_write_dispatch_test.cpp.
    emit(rom_, pc, {
        0xA5, 0x46,            // LDA $46
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,            // LDY #$00
        0xAD, dataReg, 0xC0,   // LDA $C0n3
        0x91, 0x44,            // STA ($44),Y
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xE6, 0x45,            // INC $45
        0xAD, dataReg, 0xC0,   // LDA $C0n3
        0x91, 0x44,            // STA ($44),Y
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xC6, 0x45,            // DEC $45
        0xAD, statReg, 0xC0,   // LDA $C0n4   ; status
        0x29, 0x01,            // AND #$01    ; I/O error bit
        0xD0, 0x02,            // BNE rderr
        0x18,                  // CLC         ; success
        0x60,                  // RTS
        0xA9, 0x27,            // rderr: LDA #$27 (ProDOS I/O error)
        0x38,                  // SEC
        0x60                   // RTS
    });

    // ── Write block ────────────────────────────────────────────────────
    // WP pre-check up front ($2B); after streaming 512 bytes, tests $C0n4
    // bit 0 so an out-of-range / rejected writeBlock returns carry-set
    // ($27) instead of the old unconditional CLC "success".
    emit(rom_, pc, {
        0xAD, statReg, 0xC0,   // LDA $C0n4
        0x29, 0x40,            // AND #$40    ; WP bit
        0xF0, 0x04,            // BEQ +4 (not WP → proceed)
        0xA9, 0x2B,            // LDA #$2B    ; write-protected error
        0x38,                  // SEC
        0x60,                  // RTS
        0xA5, 0x46,            // LDA $46
        0x8D, blkLoReg, 0xC0,
        0xA5, 0x47,            // LDA $47
        0x8D, blkHiReg, 0xC0,
        0xA0, 0x00,            // LDY #$00
        0xB1, 0x44,            // LDA ($44),Y
        0x8D, dataReg, 0xC0,   // STA $C0n3
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xE6, 0x45,            // INC $45
        0xB1, 0x44,            // LDA ($44),Y
        0x8D, dataReg, 0xC0,   // STA $C0n3
        0xC8,                  // INY
        0xD0, 0xF8,            // BNE -8
        0xC6, 0x45,            // DEC $45
        0xAD, statReg, 0xC0,   // LDA $C0n4   ; re-read status
        0x29, 0x01,            // AND #$01    ; I/O error bit
        0xD0, 0x04,            // BNE wrerr
        0xA9, 0x00,            // LDA #$00    ; success
        0x18,                  // CLC
        0x60,                  // RTS
        0xA9, 0x27,            // wrerr: LDA #$27 (ProDOS I/O error)
        0x38,                  // SEC
        0x60                   // RTS
    });
}

// ── MountableMediaCard ──────────────────────────────────────────────────
// Each unit is one media bay. The bay's media kind is user-selectable
// (empty / 3.5" / HDV); mounting requires a kind to have been chosen first
// (the Slot Manager surfaces the type combo next to the mount control).

MediaBayInfo SmartPortCard::bayInfo(int bay) const
{
    MediaBayInfo info;
    const SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u) return info;  // empty bay → "(empty)" type, no media
    info.kindLabel         = std::string(u->kindLabel());
    info.typeKey           = std::string(u->kindKey());
    info.path              = u->path();
    info.lastError         = u->lastError();
    info.blockCount        = u->blockCount();
    info.loaded            = u->isLoaded();
    info.writeProtected    = u->isWriteProtected();
    info.writeBackEnabled  = u->isWriteBackEnabled();
    info.supportsWriteBack = true;
    info.supportsTypeSelect = true;
    return info;
}

bool SmartPortCard::mountBay(int bay, const std::string& path,
                             std::string& errOut)
{
    SmartPortUnit* u = unit(static_cast<size_t>(bay));
    if (!u) {
        errOut = "select a media type for this unit first";
        return false;
    }
    if (!u->loadImage(path)) {
        errOut = u->lastError();
        return false;
    }
    return true;
}

void SmartPortCard::ejectBay(int bay)
{
    if (SmartPortUnit* u = unit(static_cast<size_t>(bay))) u->eject();
}

void SmartPortCard::setBayWriteBack(int bay, bool on)
{
    if (SmartPortUnit* u = unit(static_cast<size_t>(bay)))
        u->setWriteBackEnabled(on);
}

std::vector<std::pair<std::string, std::string>>
SmartPortCard::bayTypeOptions(int /*bay*/) const
{
    return {
        { std::string(),                              "(empty)" },
        { std::string(SmartPort35Unit::kKindKey),     "3.5\" 800K" },
        { std::string(SmartPortHdvUnit::kKindKey),    "ProDOS HDV" },
    };
}

void SmartPortCard::setBayType(int bay, const std::string& kindKey)
{
    if (bay < 0 || static_cast<size_t>(bay) >= kMaxUnits) return;
    if (kindKey.empty()) {
        setUnit(static_cast<size_t>(bay), nullptr);   // clear the bay
        return;
    }
    if (auto unit = makeSmartPortUnit(kindKey))
        setUnit(static_cast<size_t>(bay), std::move(unit));
}

} // namespace pom2

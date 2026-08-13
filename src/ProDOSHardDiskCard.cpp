// VERHILLE Arnaud 2026

// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ProDOSHardDiskCard.h"
#include "Logger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr uint8_t  kDriverOff  = 0x50;
constexpr uint8_t  kBootOff    = 0x20;

void emit(std::array<uint8_t, 256>& rom, uint8_t& pc, std::initializer_list<uint8_t> bytes)
{
    for (uint8_t b : bytes) rom[pc++] = b;
}

// Block-level I/O trace, gated by POM2_TRACE_HDV=1 (mirrors the env-var
// diagnostics in DiskIICard.cpp / Memory.cpp). One line per 512-byte block
// transfer — enough to see the read/write sequence around a game crash
// without drowning in 512 lines per block.
bool hdvTraceOn()
{
    // POM2_TRACE_HANG implies HDV tracing too, so a single env var captures
    // both the frozen-loop dump and the block-read sequence that led to it.
    static const bool on = std::getenv("POM2_TRACE_HDV")  != nullptr ||
                           std::getenv("POM2_TRACE_HANG") != nullptr;
    return on;
}

} // namespace

ProDOSHardDiskCard::ProDOSHardDiskCard(int slotNum)
    : slot(slotNum)
{
    buildRom();
}

bool ProDOSHardDiskCard::loadImage(const std::string& path)
{
    const bool ok = backing_.loadImage(path);
    selectedBlock = 0;
    streamOffset  = 0;
    return ok;
}

bool ProDOSHardDiskCard::loadImageFromBytes(std::vector<uint8_t> bytes,
                                            const std::string& label,
                                            const std::string& hostFolder)
{
    const bool ok = backing_.loadFromBytes(std::move(bytes), label, hostFolder);
    selectedBlock = 0;
    streamOffset  = 0;
    return ok;
}

bool ProDOSHardDiskCard::ejectImage()
{
    // Save-on-eject policy lives here (the card owns the user-facing eject):
    // flush dirty blocks first when write-back is on and the medium allows it,
    // then drop the image. backing_.saveDirty() is itself a guarded no-op.
    if (backing_.isLoaded() && backing_.hasUnsavedChanges() &&
        backing_.isWriteBackEnabled() && !backing_.isWriteProtected()) {
        if (!backing_.saveDirty()) {
            pom2::log().warn("HDV", "Save-on-eject failed: " + backing_.lastError());
            return false;
        }
    }
    backing_.eject();
    selectedBlock = 0;
    streamOffset  = 0;
    return true;
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
    //   $C0D2 write = next byte INTO selected block (write-back enabled)
    //   $C0D3 read  = bit-7 = imageLoaded, bit-6 = isWriteProtected
    if (low4 == 0x0) {
        selectedBlock = static_cast<uint16_t>((selectedBlock & 0xFF00u) | v);
        streamOffset = 0;
        if (hdvTraceOn())
            std::fprintf(stderr, "[HDV] SETLO blk=%u\n",
                         static_cast<unsigned>(selectedBlock));
    } else if (low4 == 0x1) {
        selectedBlock = static_cast<uint16_t>((selectedBlock & 0x00FFu) |
                                              (static_cast<uint16_t>(v) << 8));
        streamOffset = 0;
        if (hdvTraceOn())
            std::fprintf(stderr, "[HDV] SETHI blk=%u\n",
                         static_cast<unsigned>(selectedBlock));
    } else if (low4 == 0x2) {
        writeDataByte(v);
    }
}

uint8_t ProDOSHardDiskCard::deviceSelectRead(uint8_t low4)
{
    if (low4 == 0x2) return readDataByte();
    if (low4 == 0x3) {
        // Status byte. Preserves the original encoding for backward compat:
        //   bit-7 = 0 when image loaded, 1 when missing (legacy).
        //   bit-6 = 1 when write-protected (new — used by the write driver
        //           in the ROM to gate ProDOS WRITE_BLOCK and return $2B
        //           without touching the in-memory image).
        uint8_t s = backing_.isLoaded() ? 0x00 : 0x80;
        if (backing_.isWriteProtected()) s |= 0x40;
        return s;
    }
    if (low4 == 0x4 || low4 == 0x5) {
        // STATUS block count, low ($C0n4) / high ($C0n5). The ProDOS
        // STATUS driver call (cmd $00) must return the device's total
        // block count in X (low) / Y (high); the ROM STATUS routine
        // reads it from this register pair — same scheme (and same
        // BITSY-crash lesson) as SmartPortCard::blockCountByte. Counts
        // above $FFFF clamp (an exactly-65536-block 32 MiB image must
        // report $FFFF, not truncate to 0 = "empty volume").
        size_t blocks = backing_.isLoaded() ? backing_.blockCount() : 0u;
        if (blocks > 0xFFFFu) blocks = 0xFFFFu;
        return static_cast<uint8_t>(
            (blocks >> (low4 == 0x5 ? 8 : 0)) & 0xFF);
    }
    return 0xFF;
}

uint8_t ProDOSHardDiskCard::readDataByte()
{
    if (!backing_.isLoaded()) return 0xFF;

    if (hdvTraceOn() && streamOffset == 0) {
        const bool inRange = (selectedBlock + 1u) <= backing_.blockCount();
        std::fprintf(stderr, "[HDV] READ  blk=%u%s\n",
                     static_cast<unsigned>(selectedBlock),
                     inRange ? "" : " (OUT-OF-RANGE -> $FF)");
    }

    const size_t absolute =
        static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    const uint8_t out = backing_.readByte(absolute);
    streamOffset = (streamOffset + 1) % kBlockBytes;
    return out;
}

void ProDOSHardDiskCard::writeDataByte(uint8_t v)
{
    // Writes always land in the in-memory image so the running session sees a
    // fully writable volume (a real hard disk is read/write to ProDOS). Only
    // the real medium WP flag (2MG header) blocks the write. Persisting those
    // RAM changes to the host .hdv/.2mg file is a SEPARATE opt-in handled by
    // writeBackEnabled in saveDirty()/ejectImage().
    if (!backing_.isLoaded() || backing_.isWriteProtected()) return;

    if (hdvTraceOn() && streamOffset == 0) {
        const bool inRange = (selectedBlock + 1u) <= backing_.blockCount();
        std::fprintf(stderr, "[HDV] WRITE blk=%u wb=%d%s\n",
                     static_cast<unsigned>(selectedBlock),
                     backing_.isWriteBackEnabled() ? 1 : 0,
                     inRange ? "" : " (OUT-OF-RANGE -> dropped)");
    }

    const size_t absolute =
        static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    backing_.writeByte(absolute, v);
    streamOffset = (streamOffset + 1) % kBlockBytes;
}

void ProDOSHardDiskCard::buildRom()
{
    rom.fill(0xEA); // NOP padding

    const uint16_t kDeviceBase = static_cast<uint16_t>(0xC080 + slot * 16);
    const uint8_t  kSlotRomHi  = static_cast<uint8_t>(0xC0 + slot);
    const uint8_t  kUnitNumber = static_cast<uint8_t>(slot << 4);

    // Entry used by PR#n / direct boot: load block 0 at $0800, then jump to
    // the universal ProDOS boot loader's real entry at $0801 with X=unit.
    rom[0x00] = 0x4C;        // JMP $Cn20
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
        0x85, 0x43,       // STA $43         ; slot N, drive 1
        0xA9, 0x00,
        0x85, 0x44,       // STA $44         ; buffer low = $00
        0xA9, 0x08,
        0x85, 0x45,       // STA $45         ; buffer high = $08
        0xA9, 0x00,
        0x85, 0x46,       // STA $46         ; block low = 0
        0x85, 0x47,       // STA $47         ; block high = 0
        0x20, kDriverOff, kSlotRomHi, // JSR $Cn50
        0xB0, 0x07,       // BCS error
        0xA2, kUnitNumber,// LDX #unit
        0xA9, 0x00,       // LDA #$00
        0x4C, 0x01, 0x08, // JMP $0801
        0x4C, 0xE0, kSlotRomHi // error: JMP $CnE0 (stable halt)
    });

    // Boot error handler at $CnE0: simple infinite loop.
    // We deliberately avoid calling monitor routines here: a failed HD boot
    // should be safe even if the main ROM isn't fully initialised yet.
    rom[0xE0] = 0x4C; // JMP $CnE0
    rom[0xE1] = 0xE0;
    rom[0xE2] = kSlotRomHi;

    // Driver dispatch table (22 bytes), commands:
    //   $00 status → JMP $CnC0 (returns block count in X/Y — see below)
    //   $01 read   → branches to read block (43 bytes)
    //   $02 write  → branches to write block (47 bytes)
    //   any other  → A=$01 (bad command), SEC, RTS
    // Read block first probes $C0D3 bit-7: if set (no image mounted),
    // return $28 (NO DEVICE CONNECTED) with carry set instead of CLC
    // "success" over a $FF stream — real ProDOS drivers never report a
    // successful read from absent media.
    // Write block first probes $C0D3 bit-6: if set (image is WP), return
    // $2B (write-protected) without touching memory.
    //
    // Layout when emitted at $C550 ($Cn50):
    //   $C550..$C565  dispatch (22 B)
    //   $C566..$C590  read block (43 B)
    //   $C591..$C5BF  write block (47 B)
    //   $C5C0..$C5C9  STATUS routine (10 B)
    pc = kDriverOff;
    emit(rom, pc, {
        0xA5, 0x42,       // LDA $42         ; command
        0xC9, 0x01,       // CMP #$01
        0xF0, 0x10,       // BEQ read    (+16 → $C566)
        0xC9, 0x02,       // CMP #$02
        0xF0, 0x37,       // BEQ write   (+55 → $C591: read grew 9 B for
                          //              the no-media probe — pinned by
                          //              tests/hdv_status_driver_test.cpp)
        0xC9, 0x00,       // CMP #$00
        0xF0, 0x04,       // BEQ status  (+4  → $C562)
        0xA9, 0x01,       // LDA #$01    ; bad-command error
        0x38,             // SEC
        0x60,             // RTS
        // status: jump to the full STATUS routine at $CnC0 (returns the
        // block count in X/Y). Kept 4 bytes (JMP + NOP pad) so the BEQ
        // read/write offsets above stay valid — mirrors the identical
        // arrangement (and the BITSY crash it fixed) in
        // SmartPortCard::buildRom.
        0x4C, 0xC0, kSlotRomHi, // status: JMP $CnC0
        0xEA                    // pad
    });

    // ── STATUS routine ($CnC0) ─────────────────────────────────────────
    // ProDOS STATUS (cmd $00) must return total blocks in X (low) /
    // Y (high) so a volume scanner (BITSY, ProDOS ONLINE) can size the
    // device. The count comes from $C0n4/$C0n5 (deviceSelectRead 0x4/0x5,
    // clamped to $FFFF).
    {
        uint8_t sp = 0xC0;
        emit(rom, sp, {
            0xAE, static_cast<uint8_t>(kDeviceBase + 0x04), 0xC0, // LDX $C0n4
            0xAC, static_cast<uint8_t>(kDeviceBase + 0x05), 0xC0, // LDY $C0n5
            0xA9, 0x00,        // LDA #$00
            0x18,              // CLC
            0x60               // RTS
        });
    }

    const uint8_t dataReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t statReg = static_cast<uint8_t>(kDeviceBase + 0x03);
    emit(rom, pc, {
        0xAD, statReg, 0xC0, // read: LDA $C0D3 ; status
        0x10, 0x04,       // BPL ok          ; bit-7 set = no image
        0xA9, 0x28,       // LDA #$28        ; NO DEVICE CONNECTED
        0x38,             // SEC
        0x60,             // RTS
        0xA5, 0x46,       // ok: LDA $46     ; block low
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

    emit(rom, pc, {
        0xAD, statReg, 0xC0, // write: LDA $C0D3   ; status
        0x29, 0x40,          // AND #$40           ; WP bit
        0xF0, 0x04,          // BEQ ok
        0xA9, 0x2B,          // LDA #$2B           ; write-protected
        0x38,                // SEC
        0x60,                // RTS
        0xA5, 0x46,          // ok: LDA $46
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x00), 0xC0,
        0xA5, 0x47,          // LDA $47
        0x8D, static_cast<uint8_t>(kDeviceBase + 0x01), 0xC0,
        0xA0, 0x00,          // LDY #$00
        0xB1, 0x44,          // page 1: LDA ($44),Y
        0x8D, dataReg, 0xC0, // STA $C0D2
        0xC8,                // INY
        0xD0, 0xF8,          // BNE page 1
        0xE6, 0x45,          // INC $45
        0xB1, 0x44,          // page 2: LDA ($44),Y
        0x8D, dataReg, 0xC0, // STA $C0D2
        0xC8,                // INY
        0xD0, 0xF8,          // BNE page 2
        0xC6, 0x45,          // DEC $45
        0xA9, 0x00,          // LDA #$00
        0x18,                // CLC
        0x60                 // RTS
    });
}

// ── Snapshot / rewind ─────────────────────────────────────────────────────

namespace {
constexpr uint8_t kHdvSnapMagic[4] = { 'H', 'D', 'V', '1' };
}

void ProDOSHardDiskCard::appendSnapshotState(std::vector<uint8_t>& out) const
{
    out.insert(out.end(), kHdvSnapMagic, kHdvSnapMagic + 4);
    out.push_back(static_cast<uint8_t>(selectedBlock));
    out.push_back(static_cast<uint8_t>(selectedBlock >> 8));
    // streamOffset ∈ [0, 512]; two bytes are plenty.
    out.push_back(static_cast<uint8_t>(streamOffset));
    out.push_back(static_cast<uint8_t>(streamOffset >> 8));
}

void ProDOSHardDiskCard::loadSnapshotState(const uint8_t* data, std::size_t len)
{
    if (data == nullptr || len < 8 ||
        std::memcmp(data, kHdvSnapMagic, 4) != 0)
        return;   // foreign blob — a different card sat here
    selectedBlock = static_cast<uint16_t>(data[4] | (data[5] << 8));
    streamOffset  = static_cast<size_t>(data[6] | (data[7] << 8));
    // >=: a restored 512 would address the FIRST byte of the next block
    // before the modulo wrap, handing the guest one wrong byte (and
    // corrupting one byte of the wrong block on the write path).
    if (streamOffset >= 512) streamOffset = 0;   // untrusted input
}

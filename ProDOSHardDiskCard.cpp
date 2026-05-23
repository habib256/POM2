// POM2 Apple II Emulator
// Copyright (C) 2026

#include "ProDOSHardDiskCard.h"
#include "Logger.h"
#include "ProDOSVolume.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>

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
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        lastError = "Cannot open HDV image: " + path;
        pom2::log().warn("HDV", lastError);
        return false;
    }

    f.seekg(0, std::ios::end);
    const auto fileSize = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (fileSize == 0) {
        lastError = "HDV image is empty: " + path;
        pom2::log().warn("HDV", lastError);
        return false;
    }

    std::vector<uint8_t> bytes(fileSize);
    f.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!f) {
        lastError = "Short read on HDV image: " + path;
        pom2::log().warn("HDV", lastError);
        return false;
    }

    // 2IMG / .2mg container: 64-byte header followed by raw block data.
    // Spec: https://apple2.org.za/gswv/a2zine/Docs/DiskImage_2MG_Info.txt
    //   bytes  0..3  magic "2IMG"
    //   bytes 12..15 image format (LE u32) — 0=DOS 3.3 sector, 1=ProDOS, 2=NIB
    //   bytes 16..19 flags         (LE u32) — bit 0 = write-protected
    //   bytes 24..27 data offset   (LE u32) — typically 64
    //   bytes 28..31 data length   (LE u32) — bytes of block data following
    size_t parsedOffset = 0;
    size_t parsedLength = bytes.size();
    bool   parsedWp     = false;
    if (bytes.size() >= 64 &&
        bytes[0] == '2' && bytes[1] == 'I' && bytes[2] == 'M' && bytes[3] == 'G') {
        auto rd32 = [&](size_t o) {
            return static_cast<uint32_t>(bytes[o]) |
                   (static_cast<uint32_t>(bytes[o + 1]) << 8) |
                   (static_cast<uint32_t>(bytes[o + 2]) << 16) |
                   (static_cast<uint32_t>(bytes[o + 3]) << 24);
        };
        const uint32_t format = rd32(12);
        const uint32_t flags  = rd32(16);
        const uint32_t off    = rd32(24);
        const uint32_t len    = rd32(28);
        if (format != 1) {
            lastError = "2IMG image is not in ProDOS block order (format=" +
                        std::to_string(format) + ")";
            pom2::log().warn("HDV", lastError);
            return false;
        }
        if (off < 64 || off > bytes.size() ||
            len == 0 || static_cast<size_t>(off) + len > bytes.size()) {
            lastError = "2IMG header points outside the file (offset=" +
                        std::to_string(off) + ", length=" + std::to_string(len) + ")";
            pom2::log().warn("HDV", lastError);
            return false;
        }
        parsedOffset = off;
        parsedLength = len;
        parsedWp     = (flags & 1u) != 0;
    }

    if ((parsedLength % kBlockBytes) != 0) {
        lastError = "HDV image data is not a whole number of 512-byte blocks: " +
                    std::to_string(parsedLength);
        pom2::log().warn("HDV", lastError);
        return false;
    }
    if ((parsedLength / kBlockBytes) > 0x10000u) {
        lastError = "HDV image has more than 65536 ProDOS blocks: " +
                    std::to_string(parsedLength / kBlockBytes);
        pom2::log().warn("HDV", lastError);
        return false;
    }

    headerBytes.assign(bytes.begin(),
                       bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset));
    image.assign(bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset),
                 bytes.begin() + static_cast<std::ptrdiff_t>(parsedOffset + parsedLength));
    dataOffset = parsedOffset;
    dataLength = parsedLength;
    writeProtectedHeader = parsedWp;
    supportsWriteBack    = true;
    isSynthVolume        = false;
    hostFolderPath.clear();
    dirtyBlocks.assign(getBlockCount(), false);
    anyDirty = false;
    imagePath = path;
    imageLoaded = true;
    selectedBlock = 0;
    streamOffset = 0;

    pom2::log().info("HDV", "Loaded " + path + " (" +
                            std::to_string(getBlockCount()) + " blocks)");
    return true;
}

bool ProDOSHardDiskCard::loadImageFromBytes(std::vector<uint8_t> bytes,
                                            const std::string& label,
                                            const std::string& hostFolder)
{
    if (bytes.empty() || (bytes.size() % kBlockBytes) != 0) {
        lastError = "synthesised image is empty or not a multiple of 512";
        pom2::log().warn("HDV", lastError);
        return false;
    }
    image = std::move(bytes);
    headerBytes.clear();
    dataOffset = 0;
    dataLength = image.size();
    isSynthVolume        = !hostFolder.empty();
    hostFolderPath       = hostFolder;
    supportsWriteBack    = isSynthVolume;
    writeProtectedHeader = false;
    dirtyBlocks.assign(getBlockCount(), false);
    anyDirty = false;
    imagePath = label;
    imageLoaded = true;
    selectedBlock = 0;
    streamOffset = 0;
    pom2::log().info("HDV", "Loaded synthesised volume: " + label +
                            " (" + std::to_string(getBlockCount()) + " blocks)");
    return true;
}

void ProDOSHardDiskCard::ejectImage()
{
    if (imageLoaded && anyDirty && writeBackEnabled && !writeProtectedHeader) {
        if (!saveDirty()) {
            pom2::log().warn("HDV", "Save-on-eject failed: " + lastError);
        }
    }
    image.clear();
    headerBytes.clear();
    dirtyBlocks.clear();
    dataOffset = 0;
    dataLength = 0;
    imagePath.clear();
    hostFolderPath.clear();
    imageLoaded = false;
    isSynthVolume = false;
    supportsWriteBack = false;
    writeProtectedHeader = false;
    anyDirty = false;
    selectedBlock = 0;
    streamOffset = 0;
}

bool ProDOSHardDiskCard::saveDirty()
{
    if (!imageLoaded || !anyDirty || !writeBackEnabled
        || writeProtectedHeader || !supportsWriteBack) {
        return true;
    }

    if (isSynthVolume) {
        pom2::ProDOSDecodeResult r = pom2::decodeVolumeToFolder(image, hostFolderPath);
        if (!r.ok) {
            lastError = r.error;
            pom2::log().warn("HDV", "Synth folder write-back failed: " + lastError);
            return false;
        }
        std::fill(dirtyBlocks.begin(), dirtyBlocks.end(), false);
        anyDirty = false;
        pom2::log().info("HDV", "Synth folder write-back: " +
                                std::to_string(r.filesWritten) + " file(s) → " +
                                hostFolderPath);
        return true;
    }

    // .hdv / .2mg: in-place rewrite of dirty blocks. Open as in|out (no
    // trunc) so the 2MG header AND any trailing comment / creator chunk
    // past dataOffset+dataLength are preserved bit-for-bit.
    std::fstream f(imagePath, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        lastError = "Cannot open " + imagePath + " for write";
        pom2::log().warn("HDV", lastError);
        return false;
    }
    size_t written = 0;
    for (size_t b = 0; b < dirtyBlocks.size(); ++b) {
        if (!dirtyBlocks[b]) continue;
        f.seekp(static_cast<std::streamoff>(dataOffset + b * kBlockBytes));
        f.write(reinterpret_cast<const char*>(&image[b * kBlockBytes]),
                static_cast<std::streamsize>(kBlockBytes));
        if (!f) {
            lastError = "Short write on " + imagePath;
            pom2::log().warn("HDV", lastError);
            return false;
        }
        ++written;
    }
    f.flush();
    std::fill(dirtyBlocks.begin(), dirtyBlocks.end(), false);
    anyDirty = false;
    pom2::log().info("HDV", "Saved " + std::to_string(written) +
                            " modified block(s) to " + imagePath);
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
        //           in $Cn88 to gate ProDOS WRITE_BLOCK and return $2B
        //           without touching the in-memory image).
        uint8_t s = imageLoaded ? 0x00 : 0x80;
        if (isWriteProtected()) s |= 0x40;
        return s;
    }
    return 0xFF;
}

uint8_t ProDOSHardDiskCard::readDataByte()
{
    if (!imageLoaded) return 0xFF;

    activityTicks.store(kBusyHysteresisFrames, std::memory_order_relaxed);
    if (hdvTraceOn() && streamOffset == 0) {
        const bool inRange =
            (static_cast<size_t>(selectedBlock) + 1) * kBlockBytes <= image.size();
        std::fprintf(stderr, "[HDV] READ  blk=%u%s\n",
                     static_cast<unsigned>(selectedBlock),
                     inRange ? "" : " (OUT-OF-RANGE -> $FF)");
    }

    const size_t absolute = static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    const uint8_t out = (absolute < image.size()) ? image[absolute] : 0xFF;
    streamOffset = (streamOffset + 1) % kBlockBytes;
    return out;
}

void ProDOSHardDiskCard::writeDataByte(uint8_t v)
{
    // Writes always land in the in-memory image so the running session sees a
    // fully writable volume (a real hard disk is read/write to ProDOS). Only
    // the real medium WP flag (2MG header) blocks the write. Persisting those
    // RAM changes to the host .hdv/.2mg file is a SEPARATE opt-in handled by
    // writeBackEnabled in saveDirty()/ejectImage() — so the user's file stays
    // untouched by default while the game still works. (Previously the
    // write-back-off default also gated this, which surfaced a write-
    // protected boot volume to ProDOS and crashed games that write on the fly,
    // e.g. Nox Archaist when entering a city.)
    if (!imageLoaded || writeProtectedHeader) return;

    activityTicks.store(kBusyHysteresisFrames, std::memory_order_relaxed);
    if (hdvTraceOn() && streamOffset == 0) {
        const bool inRange =
            (static_cast<size_t>(selectedBlock) + 1) * kBlockBytes <= image.size();
        std::fprintf(stderr, "[HDV] WRITE blk=%u wb=%d%s\n",
                     static_cast<unsigned>(selectedBlock), writeBackEnabled ? 1 : 0,
                     inRange ? "" : " (OUT-OF-RANGE -> dropped)");
    }

    const size_t absolute = static_cast<size_t>(selectedBlock) * kBlockBytes + streamOffset;
    if (absolute < image.size()) {
        if (image[absolute] != v) {
            image[absolute] = v;
            if (selectedBlock < dirtyBlocks.size() && !dirtyBlocks[selectedBlock]) {
                dirtyBlocks[selectedBlock] = true;
                anyDirty = true;
            }
        }
    }
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
    //   $00 status → A=0, CLC, RTS (success, no error)
    //   $01 read   → branches to read block (34 bytes)
    //   $02 write  → branches to write block (47 bytes)
    //   any other  → A=$01 (bad command), SEC, RTS
    // Write block first probes $C0D3 bit-6: if set (image is WP), return
    // $2B (write-protected) without touching memory.
    //
    // Layout when emitted at $C550 ($Cn50):
    //   $C550..$C565  dispatch (22 B)
    //   $C566..$C587  read block (34 B)
    //   $C588..$C5B6  write block (47 B)
    pc = kDriverOff;
    emit(rom, pc, {
        0xA5, 0x42,       // LDA $42         ; command
        0xC9, 0x01,       // CMP #$01
        0xF0, 0x10,       // BEQ read    (+16 → $C566)
        0xC9, 0x02,       // CMP #$02
        0xF0, 0x2E,       // BEQ write   (+46 → $C588)
        0xC9, 0x00,       // CMP #$00
        0xF0, 0x04,       // BEQ status  (+4  → $C562)
        0xA9, 0x01,       // LDA #$01    ; bad-command error
        0x38,             // SEC
        0x60,             // RTS
        0xA9, 0x00,       // status: LDA #$00
        0x18,             // CLC
        0x60              // RTS
    });

    const uint8_t dataReg = static_cast<uint8_t>(kDeviceBase + 0x02);
    const uint8_t statReg = static_cast<uint8_t>(kDeviceBase + 0x03);
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
